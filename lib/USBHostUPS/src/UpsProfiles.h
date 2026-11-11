/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UPS_PROFILES_H
#define UPS_PROFILES_H

#include <stddef.h>
#include <stdint.h>

class UPSData;

/**
 * @brief The UPS models of a maker, as NUT ups.conf sections with a header that says which
 * model a reading is: Tescom's nut-app profiles (profiles/NAME.conf), exported to JSON by its
 * tools/export-profiles and turned into tables by scripts/gen_profiles.py
 * (UpsProfiles.gen.h).
 *
 * The firmware uses them the way the Tescom app does: a driver reads the UPS, then the
 * profile whose USB ID and Match rules fit the reading names the model and supplies the
 * settings NUT would have from ups.conf (override.*, default.*, battery limits).
 */
namespace UpsProfiles {

// A "key = value" of a section or a Match rule; value is "" for a bare flag (novendor)
struct Setting {
    const char* key;
    const char* value;
};

struct Profile {
    const char* key;       // section name: leo
    const char* model;     // # Model: Tescom Leo LCD
    const char* link;      // # Link: USB, Serial or Network
    bool has_usb_id;
    uint16_t vid;
    uint16_t pid;
    const char* driver;    // nutdrv_qx
    const char* subdriver; // hunnox, or nullptr
    const Setting* options;
    size_t option_count;
    const Setting* match;
    size_t match_count;

    // The value of an option, nullptr when the section does not set it ("" for a flag)
    const char* option(const char* key) const;
    // The model without the maker's name: "Leo LCD", as a UPS reports ups.model
    const char* shortModel() const;
};

// The maker every profile is ("Tescom")
const char* manufacturer();
size_t count();
const Profile& at(size_t i);
const Profile* find(const char* key);

// Whether a profile has this USB ID
bool knowsUsbId(uint16_t vid, uint16_t pid);

/**
 * @brief Whether a reading fits every Match rule of a profile: "driver" and "subdriver"
 * are driver.name and driver.parameter.subdriver; any other key is a variable, compared
 * as a number when both sides are numbers (24 fits 24.0), as text otherwise.
 */
bool matches(const Profile& p, const UPSData& data);

/**
 * @brief The one profile with this USB ID whose Match rules the reading fits.
 *
 * @param candidates when not null, how many profiles fit (0, 1 or more): with more than
 *        one the model cannot be told and nullptr is returned
 */
const Profile* identify(uint16_t vid, uint16_t pid, const UPSData& data, size_t* candidates = nullptr);

// The value a profile gives a variable: override.<var>, else default.<var>, else nullptr
const char* setting(const Profile& p, const char* var);
// setting() as a number; false when unset or not a number
bool number(const Profile& p, const char* var, double& out);
// battery.packs when the profile sets battery_voltage_reports_one_pack (the UPS reports
// the voltage of one pack), else 1: what nutdrv_qx multiplies battery.voltage by
int batteryPacks(const Profile& p);

/**
 * @brief Applies a profile to a reading as NUT applies ups.conf: override.<var> always,
 * default.<var> when the driver did not set it, and the maker and model names.
 *
 * Call after each poll, before the charge is estimated from the voltage.
 */
void apply(const Profile& p, UPSData& data);

} // namespace UpsProfiles

#endif // UPS_PROFILES_H
