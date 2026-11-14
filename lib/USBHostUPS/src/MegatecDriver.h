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

#ifndef MEGATEC_DRIVER_H
#define MEGATEC_DRIVER_H

#include "IUPSDriver.h"
#include "IUpsControl.h"
#include "UpsProfiles.h"

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
        // krauler_command as the Tescom LiON RT needs it (nutdrv_qx_mec: nut-app's
        // deploy/nut-patches 102-byte reads, deploy/mec0003-warmup): no lock-step, langid
        // 0x0409, 102 bytes, the reply's first byte replaced by the command's prefix
        KRAULER,
    };

    explicit MegatecDriver(Transport transport = Transport::HUNNOX, uint16_t vid = 0x0001, uint16_t pid = 0x0000)
        : _transport(transport), _start(transport), _vid(vid), _pid(pid) {}

    const char* getDriverName() const override {
        switch (_transport) {
        case Transport::HUNNOX: return "MegatecDriver (hunnox)";
        case Transport::KRAULER: return "MegatecDriver (krauler)";
        default: return "MegatecDriver (fabula)";
        }
    }
    Transport transport() const { return _transport; }
    // The maker's profile the readings were matched to (UpsProfiles), nullptr until then
    const UpsProfiles::Profile* profile() const { return _profile; }

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
    // F replies without ratings before the model is matched on the status reply alone: the
    // Neoline's F is all zeros for good, while a waking bridge fills it within seconds
    static const uint8_t F_TRIES_BEFORE_MATCH = 10;
    // Polls of hunnox without a single Q1 before trying krauler: the LEO, SmartBox and
    // Neoline answer hunnox at once, the LiON RT only krauler
    static const uint8_t HUNNOX_POLLS_BEFORE_KRAULER = 5;
    // Polls spent on krauler's warm-up (one 102-byte Q1 string request each), as UPSmart
    // and deploy/mec0003-warmup wake the LiON RT's bridge
    static const uint8_t KRAULER_WARMUP_POLLS = 3;

private:
    // String index of a Megatec command, 0 for one this transport does not know
    uint8_t indexOf(const char* cmd) const;
    // hunnox_protocol(): one step of the lock-step requests before the command at index
    void hunnoxStep(IHidTransport* host, uint8_t index);
    // NUT's driver.name / subdriver / version.data for the transport in use
    void setDriverVars(UPSData& data) const;

    // Matches the reading to a profile once the ratings are in, or once F_TRIES_BEFORE_MATCH
    // F replies came without them; again when ratings arrive after a match that found none
    void identify(IHidTransport* host, const UPSData& data);

    Transport _transport;
    Transport _start; // the transport setup() goes back to
    uint16_t _vid;
    uint16_t _pid;
    const UpsProfiles::Profile* _profile = nullptr;
    bool _identified = false;
    uint8_t _hunnox_step = 0;
    uint32_t _last_poll = 0;
    bool _polled = false;
    bool _ratings_ok = false;
    uint8_t _f_tries = 0;
    uint8_t _q1_fails = 0;
    uint8_t _warmup = 0;
    bool _q1_ok = false;
    uint8_t _buffer[256];
};

#endif // MEGATEC_DRIVER_H
