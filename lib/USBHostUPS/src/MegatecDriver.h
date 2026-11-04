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

#ifndef MEGATEC_DRIVER_H
#define MEGATEC_DRIVER_H

#include "IUPSDriver.h"
#include "IUpsControl.h"

/**
 * @brief nutdrv_qx with the Megatec protocol over the string-descriptor transports of the
 * MEC0003 bridge (0001:0000): every command is a GET_DESCRIPTOR of a string whose index
 * stands for it (Q1 0x03, F 0x0d, I 0x0c), and the answer comes back as that string.
 *
 * Implements IUPSDriver directly (ADR 0010 amends ADR 0004 for Q* drivers). Reads only:
 * no instant command and no writable variable yet, so nothing is ever sent to the UPS
 * but the queries.
 */
class MegatecDriver : public IUPSDriver {
public:
    // The string-descriptor transports of nutdrv_qx this driver speaks
    enum class Transport : uint8_t {
        // fabula_command: string index, the device's own language, the caller's length
        FABULA,
        // hunnox_command: the lock-step requests of hunnox_protocol before each command,
        // langid 0x0409 (langid_fix), at most 102 bytes, bLength checked
        HUNNOX,
    };

    explicit MegatecDriver(Transport transport = Transport::HUNNOX) : _transport(transport) {}

    const char* getDriverName() const override {
        return _transport == Transport::HUNNOX ? "MegatecDriver (hunnox)" : "MegatecDriver (fabula)";
    }
    Transport transport() const { return _transport; }

    void setup() override;
    void loop(IHidTransport* host, UPSData& data, uint32_t now) override;
    bool initialPollDone() const override { return _q1_ok; }

    // Nothing of ours comes as a report or a string the host fetched
    void decodeReport(IHidTransport*, uint8_t, uint8_t, const uint8_t*, size_t, UPSData&) override {}
    void parseStringDescriptor(IHidTransport*, uint8_t, const uint8_t*, size_t, UPSData&) override {}

    bool beeperControllable() const override { return false; }
    bool beeperMuteSupported() const override { return false; }
    bool shutdownCommandsSupported() const override { return false; }
    bool delayVariablesSupported() const override { return false; }
    const IUpsControl& control() const override;

    /**
     * @brief Sends a Megatec command and reads its reply into reply (NUL-terminated,
     * without the CR), as the subdriver's *_command() does. Empty for "UPS No Ack".
     *
     * @return false when the transfer failed or the reply was corrupted
     */
    bool command(IHidTransport* host, const char* cmd, char* reply, size_t reply_size);

    // Seconds between polls: pollinterval of nutdrv_qx (2)
    static const uint32_t POLL_MS = 2000;
    // langid_fix of the Tescom and Hunnox profiles
    static const uint16_t LANGID = 0x0409;
    // "Seems that if we inform a large buffer, the USB locks" (hunnox_command)
    static const uint16_t HUNNOX_LENGTH = 102;

private:
    // String index of a Megatec command, 0 for one this transport does not know
    uint8_t indexOf(const char* cmd) const;
    // hunnox_protocol(): one step of the lock-step requests before the command at index
    void hunnoxStep(IHidTransport* host, uint8_t index);

    Transport _transport;
    uint8_t _hunnox_step = 0;
    uint32_t _last_poll = 0;
    bool _polled = false;
    bool _ratings_ok = false;
    bool _q1_ok = false;
    uint8_t _buffer[256];
};

#endif // MEGATEC_DRIVER_H
