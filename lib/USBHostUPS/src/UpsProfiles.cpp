/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "UpsProfiles.h"
#include "UPSData.h"
#include "UpsProfiles.gen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace UpsProfiles {

namespace {

bool parseNumber(const char* s, double& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    out = strtod(s, &end);
    return end && end != s && *end == 0;
}

// The variable a Match key stands for
const char* matchVariable(const char* key) {
    if (strcmp(key, "driver") == 0) return "driver.name";
    if (strcmp(key, "subdriver") == 0) return "driver.parameter.subdriver";
    return key;
}

bool startsWith(const char* s, const char* prefix, const char** rest) {
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) != 0) return false;
    *rest = s + n;
    return true;
}

} // namespace

const char* Profile::option(const char* k) const {
    for (size_t i = 0; i < option_count; i++) {
        if (strcmp(options[i].key, k) == 0) return options[i].value;
    }
    return nullptr;
}

const char* Profile::shortModel() const {
    const char* mfr = manufacturer();
    size_t n = strlen(mfr);
    if (strncmp(model, mfr, n) == 0 && model[n] == ' ') return model + n + 1;
    return model;
}

const char* manufacturer() { return generated::MANUFACTURER; }
size_t count() { return generated::COUNT; }
const Profile& at(size_t i) { return generated::ALL[i]; }

const Profile* find(const char* key) {
    for (const auto& p : generated::ALL) {
        if (strcmp(p.key, key) == 0) return &p;
    }
    return nullptr;
}

bool knowsUsbId(uint16_t vid, uint16_t pid) {
    for (const auto& p : generated::ALL) {
        if (p.has_usb_id && p.vid == vid && p.pid == pid) return true;
    }
    return false;
}

bool matches(const Profile& p, const UPSData& data) {
    for (size_t i = 0; i < p.match_count; i++) {
        const String* have = data.find(matchVariable(p.match[i].key));
        if (!have) return false;
        double want_n, have_n;
        if (parseNumber(p.match[i].value, want_n) && parseNumber(have->c_str(), have_n)) {
            if (fabs(want_n - have_n) > 1e-6 * (fabs(want_n) + 1)) return false;
        } else if (strcmp(have->c_str(), p.match[i].value) != 0) {
            return false;
        }
    }
    return true;
}

const Profile* identify(uint16_t vid, uint16_t pid, const UPSData& data, size_t* candidates) {
    const Profile* found = nullptr;
    size_t n = 0;
    for (const auto& p : generated::ALL) {
        if (!p.has_usb_id || p.vid != vid || p.pid != pid) continue;
        if (p.match_count == 0 || !matches(p, data)) continue;
        found = &p;
        n++;
    }
    if (candidates) *candidates = n;
    return n == 1 ? found : nullptr;
}

const char* setting(const Profile& p, const char* var) {
    char key[64];
    snprintf(key, sizeof(key), "override.%s", var);
    if (const char* v = p.option(key)) return v;
    snprintf(key, sizeof(key), "default.%s", var);
    return p.option(key);
}

bool number(const Profile& p, const char* var, double& out) {
    return parseNumber(setting(p, var), out);
}

int batteryPacks(const Profile& p) {
    double packs;
    if (!p.option("battery_voltage_reports_one_pack") || !number(p, "battery.packs", packs) || packs < 1) return 1;
    return (int)packs;
}

void apply(const Profile& p, UPSData& data) {
    for (size_t i = 0; i < p.option_count; i++) {
        const char* var;
        if (startsWith(p.options[i].key, "override.", &var)) {
            data.set(var, p.options[i].value);
        } else if (startsWith(p.options[i].key, "default.", &var)) {
            if (!data.hasKey(var)) data.set(var, p.options[i].value);
        }
    }
    data.set("device.mfr", manufacturer());
    data.set("ups.mfr", manufacturer());
    data.set("device.model", p.shortModel());
    data.set("ups.model", p.shortModel());
}

} // namespace UpsProfiles
