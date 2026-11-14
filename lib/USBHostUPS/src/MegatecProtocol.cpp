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

#include "MegatecProtocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace {

// Characters from..to (inclusive) of reply, as the qx2nut rows cut them
bool field(const char* reply, size_t from, size_t to, char* out, size_t out_size) {
    size_t len = strlen(reply);
    if (to >= len || to < from || (to - from + 2) > out_size) return false;
    memcpy(out, reply + from, to - from + 1);
    out[to - from + 1] = 0;
    return true;
}

bool number(const char* reply, size_t from, size_t to, double& value) {
    char buf[16];
    if (!field(reply, from, to, buf, sizeof(buf))) return false;
    char* end = nullptr;
    value = strtod(buf, &end);
    return end != buf;
}

String format(const char* fmt, double value) {
    char buf[24];
    snprintf(buf, sizeof(buf), fmt, value);
    return String(buf);
}

void setFlag(UPSData& data, const char* key, bool on) {
    data.set(key, on ? "1" : "0");
}

} // namespace

namespace Megatec {

int decodeStringDescriptor(const uint8_t* desc, size_t received, char* out, size_t out_size) {
    if (!desc || !out || out_size == 0 || received < 2) return -1;
    // hunnox_command(): "Invalid receive size - message corrupted"
    if (desc[0] != received) return -1;
    size_t di = 0;
    for (size_t si = 2; si + 1 < received && di < out_size - 1; si += 2) {
        out[di++] = desc[si + 1] ? '?' : (char)desc[si];
    }
    out[di] = 0;
    return (int)di;
}

bool isNoAck(const char* reply) {
    return reply && strcspn(reply, "\r") == 10 && strncasecmp(reply, "UPS No Ack", 10) == 0;
}

bool parseQ1(const char* reply, UPSData& data, int battery_packs) {
    // The answer must start with '(' and hold the 8 status bits (nutdrv_qx_megatec.c)
    if (!reply || reply[0] != '(' || strlen(reply) < Q1_LENGTH - 1) return false;
    char bits[9];
    if (!field(reply, 38, 45, bits, sizeof(bits)) || strspn(bits, "01") != 8) return false;

    double vin, vfault, vout, load, freq, vbatt, temp;
    if (!number(reply, 1, 5, vin) || !number(reply, 7, 11, vfault) || !number(reply, 13, 17, vout) ||
        !number(reply, 19, 21, load) || !number(reply, 23, 26, freq) || !number(reply, 28, 31, vbatt) ||
        !number(reply, 33, 36, temp)) {
        return false;
    }
    if (battery_packs > 1) vbatt *= battery_packs;

    data.set("input.voltage", format("%.1f", vin));
    data.set("input.voltage.fault", format("%.1f", vfault));
    data.set("output.voltage", format("%.1f", vout));
    data.set("ups.load", format("%.0f", load));
    data.set("input.frequency", format("%.1f", freq));
    data.set("battery.voltage", format("%.2f", vbatt));
    data.set("ups.temperature", format("%.1f", temp));

    // blazer_process_status_bits(), bits 38..45
    setFlag(data, "ups.status.ac_present", bits[0] == '0');   // Utility Fail (Immediate)
    setFlag(data, "ups.status.battery_low", bits[1] == '1');  // Battery Low
    // Bypass/Boost or Buck Active: which one from output against input voltage
    bool trim = false, bypass = false, boost = false;
    if (bits[2] == '1' && vin > 0) {
        if (vout >= 0.5 * vin && vout < 0.95 * vin) trim = true;
        else if (vout >= 0.95 * vin && vout < 1.05 * vin) bypass = true;
        else if (vout >= 1.05 * vin && vout < 1.5 * vin) boost = true;
    }
    setFlag(data, "ups.status.trim", trim);
    setFlag(data, "ups.status.bypassauto", bypass);
    setFlag(data, "ups.status.boost", boost);
    if (bits[3] == '1') data.set("ups.alarm", "UPS selftest failed!");  // UPS Failed
    else if (data.hasKey("ups.alarm")) data.remove("ups.alarm");
    data.set("ups.type", bits[4] == '1' ? "offline / line interactive" : "online");
    setFlag(data, "ups.status.cal", bits[5] == '1');          // Test in Progress
    // Shutdown Active (bit 44) is FSD in blazer-common; the status here has no FSD, and
    // shutdown_imminent would read as LB, so it is left out
    data.set("ups.beeper.status", bits[7] == '1' ? "enabled" : "disabled");
    return true;
}

bool parseF(const char* reply, UPSData& data) {
    if (!reply || reply[0] != '#' || strlen(reply) < F_LENGTH - 1) return false;
    // Each field on its own, as nutdrv_qx skips the items it cannot read: the Leo SmartBox
    // fills only the battery ("#   .   .  12.00   . ", seen on the board 2026-11-14), and a
    // waking MEC0003 bridge answers with zeros (deploy notes of the Tescom LiON RT)
    double vnom = 0, inom = 0, bnom = 0, fnom = 0;
    bool v = number(reply, 1, 5, vnom) && vnom > 0;
    bool b = number(reply, 11, 15, bnom) && bnom > 0;
    bool f = number(reply, 17, 20, fnom) && fnom > 0;
    if (!v && !b && !f) return false;
    bool i = number(reply, 7, 9, inom) && inom > 0;

    if (v) data.set("input.voltage.nominal", format("%.0f", vnom));
    if (i) data.set("input.current.nominal", format("%.1f", inom));
    if (b) data.set("battery.voltage.nominal", format("%.1f", bnom));
    if (f) data.set("input.frequency.nominal", format("%.0f", fnom));
    return true;
}

void estimateCharge(UPSData& data, double low, double high) {
    if (!data.hasKey("battery.voltage")) return;
    // The guess needs the nominal; a profile's limits do not (the Neoline reports none)
    if (low <= 0 || high <= 0) {
        double nominal = data.hasKey("battery.voltage.nominal") ? data.getFloat("battery.voltage.nominal") : 0;
        if (nominal <= 0) return;
        if (low <= 0) low = 104 * nominal / 120;
        if (high <= 0) high = 130 * nominal / 120;
    }
    if (high <= low) return;
    data.set("battery.voltage.low", format("%.2f", low));
    data.set("battery.voltage.high", format("%.2f", high));

    double charge = 100 * (data.getFloat("battery.voltage") - low) / (high - low);
    if (charge < 0) charge = 0;
    if (charge > 100) charge = 100;
    data.set("battery.charge", format("%.0f", charge));
}

} // namespace Megatec
