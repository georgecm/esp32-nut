/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Port of tescom_prime.c (Tescom Connect, nut-app deploy/nut-drivers/tescom_prime.c,
 * driver version 0.04), reads only.
 */

#include "PrimeDriver.h"
#include "IHidTransport.h"

namespace {

// A control with no command and no writable variable: the driver only reads
class PrimeControl : public IUpsControl {
public:
    std::vector<const UPSCommandInfo*> supportedCommands(const IUpsControlPort&) const override { return {}; }
    CommandResult executeCommand(IUpsControlPort& port, const char*, const char*) const override {
        return port.isConnected() ? CommandResult::NOT_SUPPORTED : CommandResult::NOT_CONNECTED;
    }
    NutTable::SetStatus setVariable(IUpsControlPort& port, const char*, const char*) const override {
        return port.isConnected() ? NutTable::SetStatus::UNKNOWN : NutTable::SetStatus::NOT_CONNECTED;
    }
};

const char* resultName(ModbusRtu::Result r) {
    switch (r) {
    case ModbusRtu::Result::OK: return "ok";
    case ModbusRtu::Result::NO_REPLY: return "no reply";
    case ModbusRtu::Result::EXCEPTION: return "exception";
    default: return "bad request";
    }
}

} // namespace

const char* const PrimeDriver::DRIVER_VERSION = "0.04";

const IUpsControl& PrimeDriver::control() const {
    static const PrimeControl control;
    return control;
}

void PrimeDriver::setup() {
    _profile = nullptr;
    _step = STEP_MODEL;
    _in_poll = false;
    _polled = false;
    _last_poll = 0;
    _model_done = false;
    _model_tries = 0;
    _first_done = false;
}

void PrimeDriver::setDriverVars(UPSData& data) const {
    data.set("driver.name", "tescom_prime");
    data.set("driver.version.internal", DRIVER_VERSION);
}

bool PrimeDriver::runStep(IHidTransport* host, UPSData& data) {
    using ModbusRtu::Result;
    if (_step == STEP_MODEL) {
        // upsdrv_initinfo(): a missing model string is no failure
        if (_model_done || _model_tries >= MODEL_TRIES) return true;
        _model_tries++;
        uint16_t model[Prime::MODEL_REGS];
        Result r = _modbus.readRegisters(ModbusRtu::READ_HOLDING_REGISTERS, Prime::REG_MODEL, Prime::MODEL_REGS, model);
        if (r == Result::OK) {
            _port->noteExchange(true);
            char text[2 * Prime::MODEL_REGS + 1];
            Prime::decodeModel(model, text, sizeof(text));
            host->lock();
            data.set("ups.firmware.aux", text);
            host->unlock();
            _model_done = true;
        } else {
            host->logDebug(String("[Prime] model not read: ") + resultName(r));
        }
        return true;
    }

    if (_step < STEP_BITS) {
        uint16_t start = (uint16_t)((_step - STEP_INPUT) * Prime::INPUT_BLOCK);
        Result r = _modbus.readRegisters(ModbusRtu::READ_INPUT_REGISTERS, start, Prime::INPUT_BLOCK, _regs + start);
        if (r != Result::OK) {
            host->logDebug(String("[Prime] input registers from ") + String((unsigned)start) + ": " + resultName(r));
            return false;
        }
        return true;
    }

    if (_step < STEP_BEEPER) {
        uint16_t start = (uint16_t)((_step - STEP_BITS) * Prime::BITS_BLOCK);
        Result r = _modbus.readBits(start, Prime::BITS_BLOCK, _bits + start);
        if (r != Result::OK) {
            host->logDebug(String("[Prime] status bits ") + String((unsigned)start) + ": " + resultName(r));
            return false;
        }
        return true;
    }

    // STEP_BEEPER: after the commit, as read_beeper() at the end of upsdrv_updateinfo();
    // a failed read leaves ups.beeper.status as it was
    uint16_t beeper = 0;
    Result r = _modbus.readRegisters(ModbusRtu::READ_HOLDING_REGISTERS, Prime::REG_BEEPER, 1, &beeper);
    if (r == Result::OK) {
        host->lock();
        Prime::decodeBeeper(beeper, data);
        host->unlock();
    } else {
        host->logDebug(String("[Prime] beeper not read: ") + resultName(r));
    }
    return true;
}

void PrimeDriver::commit(IHidTransport* host, UPSData& data) {
    host->lock();
    Prime::setStaticInfo(data);
    Prime::decode(_regs, _bits, data);
    setDriverVars(data);
    if (!_profile) {
        _profile = UpsProfiles::identify(0x1A86, 0x7523, data);
        if (!_profile) _profile = UpsProfiles::find("prime");
    }
    if (_profile) {
        UpsProfiles::apply(*_profile, data);
        if (const char* poll = _profile->option("pollinterval")) data.set("driver.parameter.pollinterval", poll);
    }
    host->unlock();
}

void PrimeDriver::loop(IHidTransport* host, UPSData& data, uint32_t now) {
    if (!host || !_port || host->isPollingPaused()) return;
    if (!_in_poll) {
        if (_polled && (now - _last_poll) < POLL_MS) return;
        _polled = true;
        _last_poll = now;
        _in_poll = true;
        _step = STEP_MODEL;
    }

    bool counted = _step >= STEP_INPUT && _step < STEP_BEEPER;
    if (!runStep(host, data)) {
        // No reply: the poll ends here and the next one starts from the top
        _port->noteExchange(false);
        _in_poll = false;
        return;
    }
    if (counted) _port->noteExchange(true);

    _step++;
    if (_step == STEP_BEEPER) {
        commit(host, data);
        _first_done = true;
    } else if (_step == STEP_DONE) {
        _in_poll = false;
    }
}
