/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Contains code and data derived from Network UPS Tools (https://networkupstools.org/),
 * GPL-2.0-or-later, modified for esp32-nut in 2026.
 *
 * drivers/nutdrv_qx.c, drivers/nutdrv_qx_megatec.c, drivers/nutdrv_qx_blazer-common.c:
 *   Copyright (C) 2013 Daniele Pezzini <hyouko@gmail.com>
 *   and the NUT contributors.
 */

#ifndef MEGATEC_PROTOCOL_H
#define MEGATEC_PROTOCOL_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include "UPSData.h"

/**
 * @brief The Megatec (Q1) protocol of nutdrv_qx, without any transport: the replies to
 * Q1 and F as nutdrv_qx_megatec.c reads them, the status bits of blazer-common, and the
 * battery charge nutdrv_qx guesses from the voltage when the UPS does not report one.
 *
 * Pure functions on text and UPSData, so they are tested natively with captured replies.
 */
namespace Megatec {

    // Q1: "(MMM.M NNN.N PPP.P QQQ RR.R S.SS TT.T b7b6b5b4b3b2b1b0\r", 47 bytes with the CR
    static const size_t Q1_LENGTH = 47;
    // F: "#MMM.M QQQ SS.SS RR.R\r", 22 bytes with the CR
    static const size_t F_LENGTH = 22;

    /**
     * @brief A string descriptor as hunnox_command() converts it with langid_fix: bLength
     * must equal the bytes received ("size mismatch" otherwise), then UTF-16LE to ASCII,
     * '?' for a character outside it.
     *
     * @return The length written to out (NUL-terminated), -1 for a corrupted descriptor
     */
    int decodeStringDescriptor(const uint8_t* desc, size_t received, char* out, size_t out_size);

    // "UPS No Ack": the bridge's answer to a supported command, a failure for a query
    bool isNoAck(const char* reply);

    /**
     * @brief Parses a Q1 reply into input.voltage, input.voltage.fault, output.voltage,
     * ups.load, input.frequency, battery.voltage, ups.temperature, ups.type,
     * ups.beeper.status, ups.alarm and the ups.status.* flags.
     *
     * battery_packs > 1 multiplies battery.voltage, for a UPS that reports one pack
     * (battery_voltage_reports_one_pack, qx_multiply_battvolt).
     *
     * @return false for a reply that is not a Q1 answer; data is then left untouched
     */
    bool parseQ1(const char* reply, UPSData& data, int battery_packs = 1);

    /**
     * @brief Parses an F reply into input.voltage.nominal, input.current.nominal,
     * battery.voltage.nominal and input.frequency.nominal.
     *
     * @return false for a reply that is not an F answer, or one with every rating zero
     * (what a MEC0003 bridge returns while it is still waking up)
     */
    bool parseF(const char* reply, UPSData& data);

    /**
     * @brief battery.voltage.low/high from the nominal voltage (104/120 and 130/120 of it,
     * qx_initbattery), then battery.charge from battery.voltage between them (qx_battery).
     * Does nothing without battery.voltage.nominal and battery.voltage.
     */
    void estimateCharge(UPSData& data);
}

#endif // MEGATEC_PROTOCOL_H
