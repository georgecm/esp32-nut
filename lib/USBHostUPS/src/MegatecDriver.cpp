/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Contains code derived from Network UPS Tools (https://networkupstools.org/),
 * GPL-2.0-or-later, modified for esp32-nut in 2026.
 *
 * drivers/nutdrv_qx.c (fabula_command, hunnox_command, hunnox_protocol, krauler_command):
 *   Copyright (C) 2013 Daniele Pezzini <hyouko@gmail.com>
 *   and the NUT contributors.
 */

#include "MegatecDriver.h"
#include "IHidTransport.h"
#include "MegatecProtocol.h"
#include <string.h>

namespace {

// The queries of fabula_command / hunnox_command / krauler_command, with the prefix krauler
// puts back as the reply's first byte; the commands that change the UPS (Q, C, S..R.., BL on
// hunnox) are not sent by this driver yet
struct CommandIndex {
    const char* cmd;
    uint8_t index;
    char prefix;
};
const CommandIndex COMMANDS[] = {
    { "Q1", 0x03, '(' }, // Status
    { "F", 0x0d, '#' },  // Ratings
    { "I", 0x0c, '#' },  // Vendor infos
};

char prefixOf(const char* cmd) {
    for (const auto& c : COMMANDS) {
        if (strcmp(cmd, c.cmd) == 0) return c.prefix;
    }
    return 0;
}

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
    _transport = _start;
    _q1_fails = 0;
    _warmup = 0;
    _hunnox_step = 0;
    _last_poll = 0;
    _polled = false;
    _ratings_ok = false;
    _f_tries = 0;
    _q1_ok = false;
    _profile = nullptr;
    _identified = false;
}

void MegatecDriver::identify(IHidTransport* host, const UPSData& data) {
    if (_identified || (!_ratings_ok && _f_tries < F_TRIES_BEFORE_MATCH)) return;
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
    } else if (_transport == Transport::KRAULER) {
        // The MEC0003 only refreshes its reply for 102-byte requests (UPSmart's length)
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
    // krauler_command: "Replace the first byte of what we received with the correct one"
    if (_transport == Transport::KRAULER && reply[0]) reply[0] = prefixOf(cmd);
    return true;
}

void MegatecDriver::setDriverVars(UPSData& data) const {
    // The LiON RT's patched driver is installed as nutdrv_qx_mec (nut-app profiles/lion.conf)
    data.set("driver.name", _transport == Transport::KRAULER ? "nutdrv_qx_mec" : "nutdrv_qx");
    data.set("driver.parameter.subdriver", _transport == Transport::HUNNOX    ? "hunnox"
                                           : _transport == Transport::KRAULER ? "krauler"
                                                                              : "fabula");
    data.set("driver.version.data", "Megatec 0.06");
}

void MegatecDriver::loop(IHidTransport* host, UPSData& data, uint32_t now) {
    if (!host || host->isPollingPaused()) return;
    if (_polled && (now - _last_poll) < POLL_MS) return;
    _polled = true;
    _last_poll = now;

    if (_transport == Transport::KRAULER && _warmup < KRAULER_WARMUP_POLLS) {
        size_t len = 0;
        host->requestCommandString(0x03, LANGID, HUNNOX_LENGTH, _buffer, sizeof(_buffer), len);
        _warmup++;
        return;
    }

    char reply[128];
    // F is static: read until the bridge gives real ratings, then never again
    if (!_ratings_ok && command(host, "F", reply, sizeof(reply))) {
        host->lock();
        _ratings_ok = Megatec::parseF(reply, data);
        host->unlock();
        if (!_ratings_ok) {
            host->logDebug(String("[Megatec] F not usable yet: ") + reply);
            if (_f_tries < F_TRIES_BEFORE_MATCH) _f_tries++;
        } else if (_identified && !_profile) {
            // Matched on the status alone and nothing fitted: the ratings may tell
            _identified = false;
        }
    }

    bool got = command(host, "Q1", reply, sizeof(reply));
    int packs = _profile ? UpsProfiles::batteryPacks(*_profile) : 1;
    host->lock();
    bool ok = got && Megatec::parseQ1(reply, data, packs);
    if (ok) {
        setDriverVars(data);
        data.set("device.type", "ups");
    }
    host->unlock();

    if (!ok) {
        if (got) host->logDebug(String("[Megatec] Q1 not understood: ") + reply);
        // Never a status yet: the bridge may be a LiON RT's, which answers krauler, so the
        // two transports take turns until one answers (then it stays until unplugged)
        if (!_q1_ok && _start == Transport::HUNNOX && ++_q1_fails >= HUNNOX_POLLS_BEFORE_KRAULER) {
            _transport = _transport == Transport::HUNNOX ? Transport::KRAULER : Transport::HUNNOX;
            host->logDebug(String("[Megatec] no status yet, trying ") +
                           (_transport == Transport::KRAULER ? "krauler (Tescom LiON RT)" : "hunnox"));
            _q1_fails = 0;
            _warmup = 0;
            _f_tries = 0;
            _hunnox_step = 0;
        }
        return;
    }

    if (!_identified) identify(host, data);

    {
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
    _q1_ok = true;
}
