/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CH340_HOST_H
#define CH340_HOST_H

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"

/**
 * @brief A CH340 USB-serial bridge read on raw ESP-IDF usb_host client calls (the Tescom
 * Prime's USB port, 1a86:7523, a vendor-class device: neither HID nor CDC-ACM).
 *
 * A second client of the usb_host library USBHostUPS installs, next to the HID class
 * driver: every client sees every new device. The HID driver closes devices without a HID
 * interface (hid_host_device_init_attempt), this client closes those its filter refuses, so
 * each device ends up with one of the two.
 *
 * Threading, as the HID path (ADR 0008): the client task only runs the usb_host callbacks;
 * they never block, and report a device through the event callback. Everything else
 * (claim, chip set-up, transfers) runs in the caller's task (the ups_poll task), which waits
 * for the transfer callbacks the client task delivers. A transfer that does not complete in
 * time is cancelled (halt, flush, clear of its endpoint).
 */
class Ch340Host {
public:
    enum class Event : uint8_t { CONNECTED, GONE };
    // Called from the client task: must not block
    typedef void (*EventCallback)(Event event, void* arg);
    // Whether a device is ours to read
    typedef bool (*Filter)(uint16_t vid, uint16_t pid);

    Ch340Host() = default;
    ~Ch340Host() { end(); }

    // Registers the client and starts its task; usb_host must be installed. Also looks at
    // the devices enumerated before the client existed
    bool begin(Filter filter, EventCallback callback, void* arg);
    void end();

    // Poll task: claims the vendor interface and sets the chip up (baud 8N1, DTR on, RTS
    // off, as tescom_prime's ser_set_dtr(1) / ser_set_rts(0)). False on a USB error
    bool open(uint32_t baud);
    // Sets the chip up again on the claimed interface (link recovery)
    bool reinit();
    // Releases the interface and the device (after GONE, or to give it up)
    void close();

    bool isOpen() const { return _claimed; }
    uint16_t vid() const { return _vid; }
    uint16_t pid() const { return _pid; }
    // CH341_REQ_READ_VERSION
    uint8_t version() const { return _version; }

    // Serial I/O on the bulk endpoints, poll task only
    bool write(const uint8_t* data, size_t len, uint32_t timeout_ms);
    size_t read(uint8_t* buf, size_t len, uint32_t timeout_ms);
    void flushInput();

    static const uint32_t CONTROL_TIMEOUT_MS = 1000; // DEFAULT_TIMEOUT of ch341.c
    static const uint32_t WRITE_TIMEOUT_MS = 500;
    static const uint32_t CANCEL_WAIT_MS = 200;
    static const uint8_t MCR = 1 << 5; // DTR on, RTS off

private:
    static void client_task(void* arg);
    static void client_event_cb(const usb_host_client_event_msg_t* msg, void* arg);
    static void transfer_cb(usb_transfer_t* transfer);

    // Client task (or begin()): opens the device at addr and keeps it when the filter takes it
    void tryOpen(uint8_t addr);
    bool init(uint32_t baud);
    bool control(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                 uint8_t* data, uint16_t length);
    // Submits a transfer and waits for it; on a timeout cancels it. done: its semaphore
    bool run(usb_transfer_t* xfer, SemaphoreHandle_t done, volatile bool& busy, uint32_t timeout_ms, bool control);
    bool bulkIn(uint32_t timeout_ms);
    void cancel(uint8_t ep, SemaphoreHandle_t done, volatile bool& busy);
    void freeTransfers();

    Filter _filter = nullptr;
    EventCallback _callback = nullptr;
    void* _callback_arg = nullptr;

    usb_host_client_handle_t _client = nullptr;
    TaskHandle_t _task = nullptr;
    volatile bool _run = false;
    SemaphoreHandle_t _dev_mutex = nullptr;

    usb_device_handle_t _dev = nullptr;
    volatile bool _gone = false;
    uint16_t _vid = 0;
    uint16_t _pid = 0;
    uint8_t _mps0 = 8;
    bool _claimed = false;
    uint8_t _iface = 0;
    uint8_t _ep_in = 0;
    uint8_t _ep_out = 0;
    uint16_t _mps_in = 32;
    uint8_t _version = 0;
    uint32_t _baud = 9600;

    usb_transfer_t* _ctrl = nullptr;
    usb_transfer_t* _in = nullptr;
    usb_transfer_t* _out = nullptr;
    SemaphoreHandle_t _ctrl_done = nullptr;
    SemaphoreHandle_t _in_done = nullptr;
    SemaphoreHandle_t _out_done = nullptr;
    volatile bool _ctrl_busy = false;
    volatile bool _in_busy = false;
    volatile bool _out_busy = false;
    bool _last_timeout = false; // the last run() ended on its timeout

    // Bytes of the last IN transfer not read yet
    uint8_t _rx[128];
    size_t _rx_len = 0;
    size_t _rx_pos = 0;
};

#endif // CH340_HOST_H
