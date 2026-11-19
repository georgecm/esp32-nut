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
#include <stdlib.h>
#include <strings.h>

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

const char* const* commandNames(size_t& count) {
    static const char* const names[] = {
        "beeper.enable", "beeper.disable", "beeper.mute", "beeper.toggle",
        "test.battery.start.quick", "test.battery.start", "test.battery.start.deep", "test.battery.stop",
        "load.off", "load.on",
    };
    count = sizeof(names) / sizeof(names[0]);
    return names;
}

CommandResult commandWrite(const char* name, const char* param, bool beeper_muted, CommandWrite& out) {
    if (!name) return CommandResult::NOT_SUPPORTED;
    if (!strcasecmp(name, "beeper.enable") || !strcasecmp(name, "beeper.disable") || !strcasecmp(name, "beeper.mute") ||
        !strcasecmp(name, "beeper.toggle")) {
        bool muted = !strcasecmp(name, "beeper.enable") ? false : !strcasecmp(name, "beeper.toggle") ? !beeper_muted : true;
        out = { REG_BEEPER, (uint16_t)(muted ? 1 : 0), CommandKind::BEEPER };
        return CommandResult::OK;
    }
    if (!strcasecmp(name, "test.battery.start.quick")) {
        out = { REG_TEST, 2, CommandKind::TEST_START };
        return CommandResult::OK;
    }
    if (!strcasecmp(name, "test.battery.start.deep")) {
        out = { REG_TEST, 4, CommandKind::TEST_START };
        return CommandResult::OK;
    }
    if (!strcasecmp(name, "test.battery.start")) {
        // The length in minutes, 1 by default
        int minutes = param && *param ? atoi(param) : 1;
        if (minutes < 1 || minutes > 99) return CommandResult::INVALID_ARGUMENT;
        out = { REG_TEST, (uint16_t)((minutes << 8) | 5), CommandKind::TEST_START };
        return CommandResult::OK;
    }
    if (!strcasecmp(name, "test.battery.stop")) {
        out = { REG_TEST, 1, CommandKind::TEST_STOP };
        return CommandResult::OK;
    }
    // 0xF001 switches the inverter at once: with mains present, off leaves the output on
    // through bypass; on battery it cuts the output
    if (!strcasecmp(name, "load.off") || !strcasecmp(name, "load.on")) {
        out = { REG_OUTPUT, (uint16_t)(!strcasecmp(name, "load.on") ? 1 : 2), CommandKind::INVERTER };
        return CommandResult::OK;
    }
    return CommandResult::NOT_SUPPORTED;
}

void noteCommand(const CommandWrite& w, TestState& test, UPSData& data, uint32_t now_ms) {
    if (w.kind == CommandKind::BEEPER) {
        decodeBeeper(w.value, data);
    } else if (w.kind == CommandKind::TEST_STOP) {
        test.asked = false;
        test.stopped = true;
        data.set("ups.test.result", "Aborted");
    } else if (w.kind == CommandKind::TEST_START) {
        test.asked = true;
        test.asked_ms = now_ms;
        test.stopped = false;
        data.set("ups.test.result", "Requested");
    }
}

void decode(const uint16_t* r, const uint8_t* bit, UPSData& data, int lowbatt, TestState* test, uint32_t now_ms) {
    bool on_battery = false;
    bool online = false, discharging = false, charging = false, low = false, bypass = false, off = false;
    bool cal = false;
    Mode m = mode(r, bit);
    // A battery test shows as battery mode with mains still there: OL CAL, not OB. Only for a
    // test asked here: the Prime also goes to battery with mains present on overvoltage
    if (m == Mode::BATTERY && test && r[R_INPUT_V] >= TEST_INPUT && (test->testing || test->asked)) {
        online = true;
        cal = true;
        if (!test->testing && !test->stopped) data.set("ups.test.result", "In progress");
        test->testing = true;
        test->asked = false;
    } else if (test) {
        if (test->testing && !test->stopped) data.set("ups.test.result", "Done");
        test->testing = false;
        // Out of battery mode: a stop has done its work, and the next test starts afresh
        test->stopped = false;
        if (test->asked && now_ms - test->asked_ms > TEST_WAIT_MS) {
            // Taken (echoed) but never run: the Prime does that for a while after a test
            data.set("ups.test.result", "Not started (the UPS may still be recharging from the last test)");
            test->asked = false;
        }
    }
    switch (cal ? Mode::ONLINE : m) {
    case Mode::BATTERY:
        on_battery = true;
        discharging = true;
        low = r[R_CHARGE] <= lowbatt;
        break;
    case Mode::ONLINE:
        online = true;
        // 0x3E reads 3-4 while charging, 0-1 otherwise
        charging = !cal && r[R_CHARGE] < 100 && r[R_CHARGING] > 1;
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
    setFlag(data, "ups.status.cal", cal);

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
