/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Contains code derived from Network UPS Tools (https://networkupstools.org/),
 * GPL-2.0-or-later, modified for esp32-nut in 2026.
 *
 * drivers/nutdrv_qx.c (fabula_command, hunnox_command, hunnox_protocol):
 *   Copyright (C) 2013 Daniele Pezzini <hyouko@gmail.com>
 *   and the NUT contributors.
 */

#include "MegatecDriver.h"
#include "IHidTransport.h"
#include "MegatecProtocol.h"
#include <string.h>

namespace {

// The queries of fabula_command / hunnox_command; the commands that change the UPS (Q, C,
// S..R.., BL on hunnox) are not sent by this driver yet
struct CommandIndex {
    const char* cmd;
    uint8_t index;
};
const CommandIndex COMMANDS[] = {
    { "Q1", 0x03 }, // Status
    { "F", 0x0d },  // Ratings
    { "I", 0x0c },  // Vendor infos
};

// A control with no command and no writable variable: the driver only reads
class MegatecControl : public IUpsControl {
public:
    std::vector<const UPSCommandInfo*> supportedCommands(const IUpsControlPort&) const override { return {}; }
    CommandResult executeCommand(IUpsControlPort& port, const char*, const char*) const override {
        return port.isConnected() ? CommandResult::NOT_SUPPORTED : CommandResult::NOT_CONNECTED;
    }
    NutTable::SetStatus setVariable(IUpsControlPort& port, const char*, const char*) const override {
        return port.isConnected() ? NutTable::SetStatus::UNKNOWN : NutTable::SetStatus::NOT_CONNECTED;
    }
};

} // namespace

const IUpsControl& MegatecDriver::control() const {
    static const MegatecControl control;
    return control;
}

void MegatecDriver::setup() {
    _hunnox_step = 0;
    _last_poll = 0;
    _polled = false;
    _ratings_ok = false;
    _q1_ok = false;
    _profile = nullptr;
    _identified = false;
}

void MegatecDriver::identify(IHidTransport* host, const UPSData& data) {
    if (_identified || !_ratings_ok) return;
    _identified = true;
    size_t candidates = 0;
    host->lock();
    _profile = UpsProfiles::identify(_vid, _pid, data, &candidates);
    String nominal = data.get("battery.voltage.nominal");
    host->unlock();
    if (_profile) {
        host->logDebug(String("[Megatec] profile ") + _profile->key + ": " + _profile->model);
    } else {
        host->logDebug(String("[Megatec] no single profile fits (") + String((unsigned)candidates) +
                       " candidates), battery.voltage.nominal " + nominal);
    }
}

uint8_t MegatecDriver::indexOf(const char* cmd) const {
    for (const auto& c : COMMANDS) {
        if (strcmp(cmd, c.cmd) == 0) return c.index;
    }
    return 0;
}

void MegatecDriver::hunnoxStep(IHidTransport* host, uint8_t index) {
    size_t len = 0;
    switch (_hunnox_step) {
    case 0:
        // NUT asks 1026 bytes; the host caps a string descriptor request at 255
        host->requestCommandString(0x00, LANGID, 255, _buffer, sizeof(_buffer), len);
        host->requestCommandString(0x00, LANGID, 255, _buffer, sizeof(_buffer), len);
        host->requestCommandString(0x01, LANGID, 255, _buffer, sizeof(_buffer), len);
        delay(10);
        break;
    case 1:
        if (index != 0x0d) host->requestCommandString(0x0d, LANGID, HUNNOX_LENGTH, _buffer, sizeof(_buffer), len);
        break;
    case 2:
        if (index != 0x03) host->requestCommandString(0x03, LANGID, HUNNOX_LENGTH, _buffer, sizeof(_buffer), len);
        break;
    case 3:
        // 0x0c only without novendor, which the Tescom and Hunnox profiles set
        break;
    default:
        _hunnox_step = 0;
    }
    _hunnox_step++;
    if (_hunnox_step > 3) _hunnox_step = 1;
}

bool MegatecDriver::command(IHidTransport* host, const char* cmd, char* reply, size_t reply_size) {
    if (!host || !reply || reply_size == 0) return false;
    reply[0] = 0;
    uint8_t index = indexOf(cmd);
    if (!index) return false;

    size_t len = 0;
    bool ok;
    if (_transport == Transport::HUNNOX) {
        hunnoxStep(host, index);
        ok = host->requestCommandString(index, LANGID, HUNNOX_LENGTH, _buffer, sizeof(_buffer), len);
    } else {
        // usb_get_string_simple: the first language of the device
        ok = host->requestCommandString(index, LANGID, 255, _buffer, sizeof(_buffer), len);
    }
    if (!ok || len < 2) return false;
    if (Megatec::decodeStringDescriptor(_buffer, len, reply, reply_size) < 0) {
        host->logDebug(String("[Megatec] ") + cmd + ": size mismatch " + String((unsigned)len) + " / " +
                       String((unsigned)_buffer[0]));
        return false;
    }
    char* cr = strchr(reply, '\r');
    if (cr) *cr = 0;
    if (Megatec::isNoAck(reply)) reply[0] = 0;
    return true;
}

void MegatecDriver::loop(IHidTransport* host, UPSData& data, uint32_t now) {
    if (!host || host->isPollingPaused()) return;
    if (_polled && (now - _last_poll) < POLL_MS) return;
    _polled = true;
    _last_poll = now;

    char reply[128];
    // F is static: read until the bridge gives real ratings, then never again
    if (!_ratings_ok && command(host, "F", reply, sizeof(reply))) {
        host->lock();
        _ratings_ok = Megatec::parseF(reply, data);
        host->unlock();
        if (!_ratings_ok) host->logDebug(String("[Megatec] F not usable yet: ") + reply);
    }

    if (!command(host, "Q1", reply, sizeof(reply))) return;
    int packs = _profile ? UpsProfiles::batteryPacks(*_profile) : 1;
    host->lock();
    bool ok = Megatec::parseQ1(reply, data, packs);
    if (ok) {
        data.set("driver.name", "nutdrv_qx");
        data.set("driver.parameter.subdriver", _transport == Transport::HUNNOX ? "hunnox" : "fabula");
        data.set("driver.version.data", "Megatec 0.06");
        data.set("device.type", "ups");
    }
    host->unlock();

    if (ok && !_identified && _ratings_ok) identify(host, data);

    if (ok) {
        host->lock();
        double low = 0, high = 0;
        if (_profile) {
            // The first reading after the match is scaled with the profile's packs too
            int profile_packs = UpsProfiles::batteryPacks(*_profile);
            if (profile_packs != packs) Megatec::parseQ1(reply, data, profile_packs);
            UpsProfiles::apply(*_profile, data);
            UpsProfiles::number(*_profile, "battery.voltage.low", low);
            UpsProfiles::number(*_profile, "battery.voltage.high", high);
        }
        Megatec::estimateCharge(data, low, high);
        host->unlock();
    }
    if (ok) {
        _q1_ok = true;
    } else {
        host->logDebug(String("[Megatec] Q1 not understood: ") + reply);
    }
}
