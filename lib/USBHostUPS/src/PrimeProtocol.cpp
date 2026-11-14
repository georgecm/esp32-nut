/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Port of the readings of tescom_prime.c (Tescom Connect, nut-app
 * deploy/nut-drivers/tescom_prime.c, driver version 0.04; register map in nut-app
 * docs/PRIME-MODBUS.md, worked out on a Prime 1102 XL).
 */

#include "PrimeProtocol.h"
#include <stdio.h>

namespace {

String tenths(uint16_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", v / 10.0);
    return String(buf);
}

String whole(unsigned v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", v);
    return String(buf);
}

void setFlag(UPSData& data, const char* key, bool on) {
    data.set(key, on ? "1" : "0");
}

} // namespace

namespace Prime {

Mode mode(const uint16_t* r, const uint8_t* bit) {
    if (bit[BIT_BATTERY] || (!bit[BIT_ONLINE] && !bit[BIT_BYPASS] && !bit[BIT_STANDBY] && r[R_MODE] == 9)) {
        return Mode::BATTERY;
    }
    if (bit[BIT_ONLINE] || (!bit[BIT_BYPASS] && !bit[BIT_STANDBY] && r[R_MODE] == 10)) return Mode::ONLINE;
    if (bit[BIT_BYPASS] || r[R_MODE] == 8) return Mode::BYPASS;
    return Mode::OFF;
}

void setStaticInfo(UPSData& data, int lowbatt) {
    data.set("device.mfr", "Tescom");
    data.set("device.model", "Prime"); // the family: its rating comes from the serial number
    data.set("device.type", "ups");
    data.set("ups.mfr", "Tescom");
    data.set("ups.model", "Prime");
    data.set("ups.type", "online");
    data.set("battery.voltage.nominal", "48");
    data.set("input.voltage.nominal", "230");
    data.set("output.voltage.nominal", "230");
    data.set("ups.power.nominal", "2000");
    data.set("ups.realpower.nominal", "2000");
    data.set("battery.charge.low", whole((unsigned)lowbatt));
    if (!data.hasKey("ups.test.result")) data.set("ups.test.result", "No test initiated");
}

void decode(const uint16_t* r, const uint8_t* bit, UPSData& data, int lowbatt) {
    bool on_battery = false;
    bool online = false, discharging = false, charging = false, low = false, bypass = false, off = false;
    switch (mode(r, bit)) {
    case Mode::BATTERY:
        on_battery = true;
        discharging = true;
        low = r[R_CHARGE] <= lowbatt;
        break;
    case Mode::ONLINE:
        online = true;
        // 0x3E reads 3-4 while charging, 0-1 otherwise
        charging = r[R_CHARGE] < 100 && r[R_CHARGING] > 1;
        break;
    case Mode::BYPASS:
        online = true;
        bypass = true;
        break;
    case Mode::OFF:
        off = true;
        break;
    }

    // ups.status from the flags UPSData::computeUPSStatusString() reads. OFF alone, as the
    // C driver: without ac_present it is neither OL nor OB
    if (off) {
        data.remove("ups.status.ac_present");
    } else {
        setFlag(data, "ups.status.ac_present", online);
    }
    setFlag(data, "ups.status.discharging", discharging);
    setFlag(data, "ups.status.charging", charging);
    // CHRG whenever the C driver sets it, also at 0 % (the fallback on battery.charge
    // would leave it out there)
    setFlag(data, "ups.status.fully_charged", !charging);
    setFlag(data, "ups.status.battery_low", low);
    setFlag(data, "ups.status.bypassauto", bypass);
    setFlag(data, "ups.status.off", off);
    setFlag(data, "ups.status.cal", false);

    data.set("input.voltage", tenths(r[R_INPUT_V]));
    data.set("input.frequency", r[R_INPUT_V] >= 500 ? tenths(r[R_INPUT_HZ]) : String("0.0"));
    data.set("input.bypass.voltage", tenths(r[R_BYPASS_V]));
    data.set("output.voltage", tenths(r[R_OUTPUT_V]));
    data.set("output.frequency", tenths(r[R_OUTPUT_HZ]));
    data.set("battery.voltage", tenths(r[R_BATTERY_V]));
    data.set("battery.charge", whole(r[R_CHARGE]));
    // 0x0B is minutes left on battery; on mains it means something else
    if (on_battery) {
        data.set("battery.runtime", whole(r[R_RUNTIME] * 60U));
    } else if (data.hasKey("battery.runtime")) {
        data.remove("battery.runtime");
    }
    data.set("ups.load", whole(r[R_LOAD]));
    data.set("ups.realpower", whole(r[R_REALPOWER]));
    data.markRealPowerMeasured();
    data.set("ups.power", whole(r[R_POWER]));
    data.set("ups.temperature", whole(r[R_TEMPERATURE]));
}

void decodeBeeper(uint16_t value, UPSData& data) {
    data.set("ups.beeper.status", value == 1 ? "muted" : "enabled");
}

void decodeModel(const uint16_t* regs, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t n = 0;
    for (uint16_t i = 0; i < MODEL_REGS && n + 1 < out_size; i++) {
        out[n++] = (char)(regs[i] >> 8);
        if (n + 1 < out_size) out[n++] = (char)(regs[i] & 0xFF);
    }
    out[n] = 0;
}

} // namespace Prime
