/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Port of tescom_prime.c (Tescom Connect, nut-app deploy/nut-drivers/tescom_prime.c,
 * driver version 0.04), reads only.
 */

#ifndef PRIME_DRIVER_H
#define PRIME_DRIVER_H

#include "IUPSDriver.h"
#include "IUpsControl.h"
#include "ISerialPort.h"
#include "ModbusRtu.h"
#include "PrimeProtocol.h"
#include "UpsProfiles.h"

/**
 * @brief tescom_prime: the Tescom Prime (EAST EA900 G4) over Modbus RTU, 9600 8N1, slave 1,
 * through the CH340 USB-serial bridge in the UPS (1a86:7523).
 *
 * Every POLL_MS it reads input registers 0x00-0x4F (16 at a time), discrete inputs 0-127
 * (64 at a time), commits ups.status and the readings, then reads the beeper register;
 * the model string (holding 0xA1-0xA7) once per connection. One Modbus request per loop()
 * call, so a poll step blocks the poll task for at most one request with its retries.
 *
 * A request that gets no reply ends the poll, as dstate_datastale() in the C driver: the
 * host's link monitor (ISerialPort::noteExchange) turns the data stale.
 *
 * Reads only: the instant commands of the C driver (beeper.*, test.battery.*, load.off,
 * load.on) are not ported yet, so nothing but reads is ever sent to the UPS.
 */
class PrimeDriver : public IUPSDriver {
public:
    explicit PrimeDriver(ISerialPort* port) : _port(port), _modbus(*port, Prime::SLAVE) {}

    const char* getDriverName() const override { return "PrimeDriver (tescom_prime)"; }

    void setup() override;
    void loop(IHidTransport* host, UPSData& data, uint32_t now) override;
    bool initialPollDone() const override { return _first_done; }

    // Nothing of ours comes as a HID report or string
    void decodeReport(IHidTransport*, uint8_t, uint8_t, const uint8_t*, size_t, UPSData&) override {}
    void parseStringDescriptor(IHidTransport*, uint8_t, const uint8_t*, size_t, UPSData&) override {}

    bool beeperControllable() const override { return false; }
    bool beeperMuteSupported() const override { return false; }
    bool shutdownCommandsSupported() const override { return false; }
    bool delayVariablesSupported() const override { return false; }
    const IUpsControl& control() const override;

    // The profile applied after each poll ("prime")
    const UpsProfiles::Profile* profile() const { return _profile; }

    // pollinterval of the prime profile
    static const uint32_t POLL_MS = 2000;
    // Tries of the model read, one per poll, before it is left out (the C driver reads it once)
    static const uint8_t MODEL_TRIES = 3;
    static const char* const DRIVER_VERSION; // of tescom_prime.c

private:
    enum Step : uint8_t {
        STEP_MODEL,
        STEP_INPUT,  // INPUT_REGS / INPUT_BLOCK requests
        STEP_BITS = STEP_INPUT + Prime::INPUT_REGS / Prime::INPUT_BLOCK,
        STEP_BEEPER = STEP_BITS + Prime::STATUS_BITS / Prime::BITS_BLOCK,
        STEP_DONE,
    };

    // One request of the poll; false ends it
    bool runStep(IHidTransport* host, UPSData& data);
    void commit(IHidTransport* host, UPSData& data);
    void setDriverVars(UPSData& data) const;

    ISerialPort* _port;
    ModbusRtu::Master _modbus;
    const UpsProfiles::Profile* _profile = nullptr;
    uint16_t _regs[Prime::INPUT_REGS] = {};
    uint8_t _bits[Prime::STATUS_BITS] = {};
    uint8_t _step = STEP_MODEL;
    bool _in_poll = false;
    bool _polled = false;
    uint32_t _last_poll = 0;
    bool _model_done = false;
    uint8_t _model_tries = 0;
    bool _first_done = false;
};

#endif // PRIME_DRIVER_H
