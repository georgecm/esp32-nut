#include "network/mdns_announcer.h"
#include "core/app_logger.h"

#ifdef ENABLE_MDNS
#include <ESPmDNS.h>
#include <WiFi.h>
#ifdef MDNS_SHARED_NAME
#include <mdns.h>
#endif
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

void MdnsAnnouncer::loop(bool connected, const String& hostname, const String& mfr, const String& model,
                         uint16_t nut_port) {
#ifdef ENABLE_MDNS
    if (!connected || hostname.length() == 0) return;
    if (_started && hostname != _hostname) {
        MDNS.end();
        _started = false;
        _shared_checked = false;
        _shared_ip = 0;
    }
    if (!_started) {
        if (!MDNS.begin(hostname.c_str())) {
            AppLogger::log("WARN", "[MDNS] Responder failed to start");
            return;
        }
        MDNS.addService("nut", "tcp", nut_port);
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("nut", "tcp", "fw", FIRMWARE_VERSION);
        _started = true;
        _hostname = hostname;
        _txt_model = "";
        _txt_mfr = "";
        AppLogger::log("INFO", "[MDNS] %s.local, _nut._tcp on %u", hostname.c_str(), (unsigned)nut_port);
    }
    shareName();
    if (mfr != _txt_mfr) {
        MDNS.addServiceTxt("nut", "tcp", "vendor", mfr.c_str());
        _txt_mfr = mfr;
    }
    if (model != _txt_model) {
        MDNS.addServiceTxt("nut", "tcp", "model", model.c_str());
        _txt_model = model;
    }
#else
    (void)connected;
    (void)hostname;
    (void)mfr;
    (void)model;
    (void)nut_port;
#endif
}

void MdnsAnnouncer::shareName() {
#if defined(ENABLE_MDNS) && defined(MDNS_SHARED_NAME)
    uint32_t ip = (uint32_t)WiFi.localIP();
    if (ip == 0) return;
    if (!_shared_checked) {
        _shared_checked = true;
        // Another board answers already: it keeps the name (blocks up to 1.5 s, once)
        IPAddress other = MDNS.queryHost(MDNS_SHARED_NAME, 1500);
        if ((uint32_t)other != 0 && (uint32_t)other != ip) {
            AppLogger::log("INFO", "[MDNS] %s.local is %s, not this board", MDNS_SHARED_NAME,
                           other.toString().c_str());
            return;
        }
    } else if (_shared_ip == 0 || _shared_ip == ip) {
        return;  // not ours, or nothing changed
    }
    mdns_ip_addr_t addr = {};
    addr.addr.type = ESP_IPADDR_TYPE_V4;
    addr.addr.u_addr.ip4.addr = ip;
    addr.next = nullptr;
    esp_err_t err = _shared_ip ? mdns_delegate_hostname_set_address(MDNS_SHARED_NAME, &addr)
                               : mdns_delegate_hostname_add(MDNS_SHARED_NAME, &addr);
    if (err != ESP_OK) {
        AppLogger::log("WARN", "[MDNS] %s.local not added (%d)", MDNS_SHARED_NAME, (int)err);
        return;
    }
    _shared_ip = ip;
    AppLogger::log("INFO", "[MDNS] Also %s.local", MDNS_SHARED_NAME);
#endif
}

String MdnsAnnouncer::sharedName() const {
#if defined(ENABLE_MDNS) && defined(MDNS_SHARED_NAME)
    if (_shared_ip) return String(MDNS_SHARED_NAME) + ".local";
#endif
    return String();
}
