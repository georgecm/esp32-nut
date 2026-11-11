#include "network/mdns_announcer.h"
#include "core/app_logger.h"

#ifdef ENABLE_MDNS
#include <ESPmDNS.h>
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
