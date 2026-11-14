/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Ch340Host.h"
#include "Ch340Init.h"
#include "usb/usb_helpers.h"
#include "esp_log.h"
#include <Arduino.h>
#include <string.h>

static const char* TAG = "ch340";

bool Ch340Host::begin(Filter filter, EventCallback callback, void* arg) {
    if (_client) return true;
    _filter = filter;
    _callback = callback;
    _callback_arg = arg;

    if (!_dev_mutex) _dev_mutex = xSemaphoreCreateMutex();
    if (!_ctrl_done) _ctrl_done = xSemaphoreCreateBinary();
    if (!_in_done) _in_done = xSemaphoreCreateBinary();
    if (!_out_done) _out_done = xSemaphoreCreateBinary();
    if (!_dev_mutex || !_ctrl_done || !_in_done || !_out_done) return false;

    // Control: setup packet + the 2 bytes of the version read, rounded up to an MPS of 64
    if (!_ctrl && usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + 64, 0, &_ctrl) != ESP_OK) return false;
    if (!_in && usb_host_transfer_alloc(sizeof(_rx), 0, &_in) != ESP_OK) return false;
    if (!_out && usb_host_transfer_alloc(64, 0, &_out) != ESP_OK) return false;

    usb_host_client_config_t config = {};
    config.is_synchronous = false;
    config.max_num_event_msg = 5;
    config.async.client_event_callback = Ch340Host::client_event_cb;
    config.async.callback_arg = this;
    if (usb_host_client_register(&config, &_client) != ESP_OK) {
        _client = nullptr;
        freeTransfers();
        return false;
    }

    _run = true;
    if (xTaskCreatePinnedToCore(Ch340Host::client_task, "usb_ch340", 4096, this, 5, &_task, tskNO_AFFINITY) != pdPASS) {
        _run = false;
        _task = nullptr;
        usb_host_client_deregister(_client);
        _client = nullptr;
        freeTransfers();
        return false;
    }

    // A device that enumerated before this client registered sends no NEW_DEV to it
    uint8_t addrs[8];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) == ESP_OK) {
        for (int i = 0; i < n; i++) tryOpen(addrs[i]);
    }
    return true;
}

void Ch340Host::end() {
    if (!_client) return;
    close();
    _run = false;
    usb_host_client_unblock(_client);
    for (int i = 0; i < 50 && _task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!_task && usb_host_client_deregister(_client) == ESP_OK) {
        _client = nullptr;
        freeTransfers();
    }
}

void Ch340Host::freeTransfers() {
    if (_ctrl) { usb_host_transfer_free(_ctrl); _ctrl = nullptr; }
    if (_in) { usb_host_transfer_free(_in); _in = nullptr; }
    if (_out) { usb_host_transfer_free(_out); _out = nullptr; }
}

void Ch340Host::client_task(void* arg) {
    Ch340Host* self = static_cast<Ch340Host*>(arg);
    while (self->_run) {
        usb_host_client_handle_events(self->_client, pdMS_TO_TICKS(100));
    }
    self->_task = nullptr;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Client task context: nothing here may wait for a transfer
// ---------------------------------------------------------------------------

void Ch340Host::client_event_cb(const usb_host_client_event_msg_t* msg, void* arg) {
    Ch340Host* self = static_cast<Ch340Host*>(arg);
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        self->tryOpen(msg->new_dev.address);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (msg->dev_gone.dev_hdl == self->_dev && !self->_gone) {
            self->_gone = true;
            if (self->_callback) self->_callback(Event::GONE, self->_callback_arg);
        }
    }
}

void Ch340Host::tryOpen(uint8_t addr) {
    if (xSemaphoreTake(_dev_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (_dev) {
        xSemaphoreGive(_dev_mutex);
        return; // one serial UPS at a time
    }
    usb_device_handle_t dev = nullptr;
    if (usb_host_device_open(_client, addr, &dev) != ESP_OK) {
        xSemaphoreGive(_dev_mutex);
        return;
    }
    const usb_device_desc_t* desc = nullptr;
    if (usb_host_get_device_descriptor(dev, &desc) != ESP_OK || !desc || !_filter ||
        !_filter(desc->idVendor, desc->idProduct)) {
        usb_host_device_close(_client, dev);
        xSemaphoreGive(_dev_mutex);
        return;
    }
    _dev = dev;
    _vid = desc->idVendor;
    _pid = desc->idProduct;
    _mps0 = desc->bMaxPacketSize0 ? desc->bMaxPacketSize0 : 8;
    _gone = false;
    xSemaphoreGive(_dev_mutex);
    ESP_LOGI(TAG, "%04X:%04X at address %u", _vid, _pid, addr);
    if (_callback) _callback(Event::CONNECTED, _callback_arg);
}

void Ch340Host::transfer_cb(usb_transfer_t* transfer) {
    Ch340Host* self = static_cast<Ch340Host*>(transfer->context);
    if (transfer == self->_ctrl) {
        self->_ctrl_busy = false;
        xSemaphoreGive(self->_ctrl_done);
    } else if (transfer == self->_in) {
        self->_in_busy = false;
        xSemaphoreGive(self->_in_done);
    } else if (transfer == self->_out) {
        self->_out_busy = false;
        xSemaphoreGive(self->_out_done);
    }
}

// ---------------------------------------------------------------------------
// Caller (poll task) context
// ---------------------------------------------------------------------------

bool Ch340Host::open(uint32_t baud) {
    if (!_dev || _gone) return false;
    if (!_claimed) {
        const usb_config_desc_t* cfg = nullptr;
        if (usb_host_get_active_config_descriptor(_dev, &cfg) != ESP_OK || !cfg) return false;
        // The first interface with a bulk IN and a bulk OUT endpoint (interface 0 on the
        // CH340: class 0xFF, bulk 0x82 / 0x02 and an interrupt IN for the modem status)
        bool found = false;
        int offset = 0;
        const usb_standard_desc_t* d = (const usb_standard_desc_t*)cfg;
        while (!found && (d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset))) {
            const usb_intf_desc_t* intf = (const usb_intf_desc_t*)d;
            if (intf->bAlternateSetting != 0) continue;
            uint8_t in = 0, out = 0;
            uint16_t mps_in = 0;
            for (int i = 0; i < intf->bNumEndpoints; i++) {
                int ep_offset = offset;
                const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &ep_offset);
                if (!ep || (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK) continue;
                if (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                    in = ep->bEndpointAddress;
                    mps_in = USB_EP_DESC_GET_MPS(ep);
                } else {
                    out = ep->bEndpointAddress;
                }
            }
            if (in && out) {
                _iface = intf->bInterfaceNumber;
                _ep_in = in;
                _ep_out = out;
                _mps_in = mps_in ? mps_in : 32;
                found = true;
            }
        }
        if (!found || _mps_in > sizeof(_rx)) {
            ESP_LOGW(TAG, "no bulk interface");
            return false;
        }
        esp_err_t err = usb_host_interface_claim(_client, _dev, _iface, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "interface %u not claimed: %s", _iface, esp_err_to_name(err));
            return false;
        }
        _claimed = true;
    }
    _rx_len = _rx_pos = 0;
    return init(baud);
}

bool Ch340Host::reinit() {
    if (!_claimed || _gone) return false;
    _rx_len = _rx_pos = 0;
    return init(_baud);
}

bool Ch340Host::init(uint32_t baud) {
    _baud = baud;
    // ch341_configure(): "expect two bytes 0x27 0x00"
    uint8_t buf[2] = {0, 0};
    if (!control(Ch340::REQUEST_IN, Ch340::REQ_READ_VERSION, 0, 0, buf, sizeof(buf))) return false;
    _version = buf[0];

    Ch340::Request reqs[4];
    size_t n = Ch340::serialInit(_version, baud, Ch340::LCR_8N1, MCR, reqs, 4);
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++) {
        if (!control(Ch340::REQUEST_OUT, reqs[i].bRequest, reqs[i].wValue, reqs[i].wIndex, nullptr, 0)) return false;
    }
    ESP_LOGI(TAG, "version 0x%02X, %u baud 8N1, DTR on, RTS off", _version, (unsigned)baud);
    return true;
}

void Ch340Host::close() {
    // Pending transfers first, without the mutex: their callbacks come from the client task,
    // which takes the mutex for a new device
    if (_claimed) {
        if (_in_busy) cancel(_ep_in, _in_done, _in_busy);
        if (_out_busy) cancel(_ep_out, _out_done, _out_busy);
    }
    if (xSemaphoreTake(_dev_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (_dev) {
        if (_claimed) usb_host_interface_release(_client, _dev, _iface);
        usb_host_device_close(_client, _dev);
    }
    _claimed = false;
    _dev = nullptr;
    _gone = false;
    _rx_len = _rx_pos = 0;
    xSemaphoreGive(_dev_mutex);
}

bool Ch340Host::control(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                        uint8_t* data, uint16_t length) {
    if (!_dev || _gone || !_ctrl || _ctrl_busy) return false;
    usb_setup_packet_t* setup = (usb_setup_packet_t*)_ctrl->data_buffer;
    setup->bmRequestType = request_type;
    setup->bRequest = request;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = length;
    bool in = (request_type & 0x80) != 0;
    size_t data_len = in ? usb_round_up_to_mps(length, _mps0) : length;
    if (USB_SETUP_PACKET_SIZE + data_len > _ctrl->data_buffer_size) return false;
    if (!in && length && data) memcpy(_ctrl->data_buffer + USB_SETUP_PACKET_SIZE, data, length);
    _ctrl->num_bytes = USB_SETUP_PACKET_SIZE + data_len;
    _ctrl->device_handle = _dev;
    _ctrl->bEndpointAddress = 0;
    _ctrl->callback = Ch340Host::transfer_cb;
    _ctrl->context = this;
    _ctrl->timeout_ms = CONTROL_TIMEOUT_MS;

    if (!run(_ctrl, _ctrl_done, _ctrl_busy, CONTROL_TIMEOUT_MS, true)) {
        ESP_LOGW(TAG, "request 0x%02X failed", request);
        return false;
    }
    if (in && data) {
        int got = _ctrl->actual_num_bytes - USB_SETUP_PACKET_SIZE;
        if (got < (int)length) return false;
        memcpy(data, _ctrl->data_buffer + USB_SETUP_PACKET_SIZE, length);
    }
    return true;
}

bool Ch340Host::run(usb_transfer_t* xfer, SemaphoreHandle_t done, volatile bool& busy, uint32_t timeout_ms, bool control) {
    xSemaphoreTake(done, 0); // a stale completion
    _last_timeout = false;
    busy = true;
    esp_err_t err = control ? usb_host_transfer_submit_control(_client, xfer) : usb_host_transfer_submit(xfer);
    if (err != ESP_OK) {
        busy = false;
        return false;
    }
    if (xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        _last_timeout = true;
        if (control) {
            // EP0 cannot be halted from here: the transfer stays ours until its callback, and
            // every control request fails meanwhile (the host's link monitor takes over)
            return false;
        }
        cancel(xfer->bEndpointAddress, done, busy);
        // Completed while being cancelled: keep what came
        return xfer->status == USB_TRANSFER_STATUS_COMPLETED && !busy;
    }
    return xfer->status == USB_TRANSFER_STATUS_COMPLETED;
}

void Ch340Host::cancel(uint8_t ep, SemaphoreHandle_t done, volatile bool& busy) {
    if (!_dev) return;
    usb_host_endpoint_halt(_dev, ep);
    usb_host_endpoint_flush(_dev, ep);
    usb_host_endpoint_clear(_dev, ep);
    // The flushed transfer comes back through its callback (CANCELED)
    if (busy) xSemaphoreTake(done, pdMS_TO_TICKS(CANCEL_WAIT_MS));
}

bool Ch340Host::bulkIn(uint32_t timeout_ms) {
    _rx_len = _rx_pos = 0;
    if (!_claimed || _gone || _in_busy) return false;
    _in->num_bytes = _mps_in; // an IN transfer is a whole number of packets
    _in->device_handle = _dev;
    _in->bEndpointAddress = _ep_in;
    _in->callback = Ch340Host::transfer_cb;
    _in->context = this;
    _in->timeout_ms = timeout_ms;
    bool ok = run(_in, _in_done, _in_busy, timeout_ms, false);
    // The CH340 answers an IN with the bytes it holds, as a short packet: a cancelled
    // transfer may still carry some
    if (!_in_busy && _in->actual_num_bytes > 0 &&
        (ok || _in->status == USB_TRANSFER_STATUS_CANCELED)) {
        _rx_len = (size_t)_in->actual_num_bytes;
        if (_rx_len > sizeof(_rx)) _rx_len = sizeof(_rx);
        memcpy(_rx, _in->data_buffer, _rx_len);
    }
    return ok;
}

bool Ch340Host::write(const uint8_t* data, size_t len, uint32_t timeout_ms) {
    if (!_claimed || _gone || !data || len == 0 || len > _out->data_buffer_size || _out_busy) return false;
    memcpy(_out->data_buffer, data, len);
    _out->num_bytes = len;
    _out->device_handle = _dev;
    _out->bEndpointAddress = _ep_out;
    _out->callback = Ch340Host::transfer_cb;
    _out->context = this;
    _out->timeout_ms = timeout_ms;
    return run(_out, _out_done, _out_busy, timeout_ms, false) && _out->actual_num_bytes == (int)len;
}

size_t Ch340Host::read(uint8_t* buf, size_t len, uint32_t timeout_ms) {
    if (!buf) return 0;
    size_t got = 0;
    uint32_t start = millis();
    while (got < len) {
        if (_rx_pos < _rx_len) {
            size_t n = _rx_len - _rx_pos;
            if (n > len - got) n = len - got;
            memcpy(buf + got, _rx + _rx_pos, n);
            _rx_pos += n;
            got += n;
            continue;
        }
        uint32_t elapsed = millis() - start;
        if (elapsed >= timeout_ms || !_claimed || _gone) break;
        bool ok = bulkIn(timeout_ms - elapsed);
        // A failed submit or transfer (stall, device gone, transfer still held) would spin
        if (_rx_len == 0 && ((!ok && !_last_timeout) || _in_busy || _gone)) break;
    }
    return got;
}

void Ch340Host::flushInput() {
    _rx_len = _rx_pos = 0;
    // What the chip holds is only handed over on an IN transfer: drain it briefly
    for (int i = 0; i < 4 && _claimed && !_gone; i++) {
        bulkIn(5);
        if (_rx_len == 0) break;
    }
    _rx_len = _rx_pos = 0;
}
