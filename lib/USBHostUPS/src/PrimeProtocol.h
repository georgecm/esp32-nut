/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Port of the readings of tescom_prime.c (Tescom Connect, nut-app
 * deploy/nut-drivers/tescom_prime.c, driver version 0.04; register map in nut-app
 * docs/PRIME-MODBUS.md, worked out on a Prime 1102 XL).
 */

#ifndef PRIME_PROTOCOL_H
#define PRIME_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include "UPSData.h"
#include "UpsCommands.h"

/**
 * @brief The Tescom Prime (an EAST EA900 G4): Modbus RTU registers turned into NUT
 * variables as tescom_prime does. No I/O here: PrimeDriver reads, this decodes.
 */
namespace Prime {

static const uint8_t SLAVE = 1;
static const uint16_t INPUT_REGS = 0x50;  // input registers read each poll: 0x00-0x4F
static const uint16_t INPUT_BLOCK = 16;   // read 16 at a time, as upsdrv_updateinfo()
static const uint16_t STATUS_BITS = 128;  // discrete inputs read each poll: 0-127
static const uint16_t BITS_BLOCK = 64;
static const uint16_t REG_BEEPER = 0x13;  // holding: 1 muted, 0 sounding
static const uint16_t REG_MODEL = 0xA1;   // holding 0xA1-0xA7: "EA900_1-3K_G4"
static const uint16_t MODEL_REGS = 7;
// Holding registers written by the commands (function 06), docs/PRIME-MODBUS.md "Commands"
static const uint16_t REG_TEST = 0xF009;   // 1 cancel, 2 quick, 4 to low battery, (n << 8) | 5 n minutes
static const uint16_t REG_OUTPUT = 0xF001; // inverter: 1 on, 2 off at once (no delay)
// Input (0.1 V) at or above which battery mode is a test, not a cut
static const uint16_t TEST_INPUT = 1700;
// A test may take this long to start (46 s seen on 2026-11-12)
static const uint32_t TEST_WAIT_MS = 120000;
// LB at or below this charge on battery (lowbatt of the C driver, default 20)
static const int LOWBATT = 20;

// Input registers (function 04)
static const uint16_t R_INPUT_V = 0x05;     // x0.1 V
static const uint16_t R_INPUT_HZ = 0x06;    // x0.1 Hz
static const uint16_t R_OUTPUT_V = 0x07;    // x0.1 V
static const uint16_t R_OUTPUT_HZ = 0x08;   // x0.1 Hz
static const uint16_t R_BATTERY_V = 0x0A;   // x0.1 V
static const uint16_t R_RUNTIME = 0x0B;     // minutes left on battery (on mains: something else)
static const uint16_t R_POWER = 0x0C;       // output VA
static const uint16_t R_REALPOWER = 0x0E;   // output W
static const uint16_t R_LOAD = 0x10;        // % of 2000 VA
static const uint16_t R_TEMPERATURE = 0x24; // deg C
static const uint16_t R_CHARGE = 0x2E;      // %
static const uint16_t R_BYPASS_V = 0x34;    // x0.1 V
static const uint16_t R_CHARGING = 0x3E;    // 3-4 while charging, 0-1 otherwise
static const uint16_t R_MODE = 0x48;        // 8 bypass, 9 battery, 10 online

// Discrete inputs (function 02): one of them is set at a time
static const uint16_t BIT_STANDBY = 120;    // standby, no mains (output off)
static const uint16_t BIT_ONLINE = 121;
static const uint16_t BIT_BATTERY = 122;
static const uint16_t BIT_BYPASS = 123;

enum class Mode : uint8_t { BATTERY, ONLINE, BYPASS, OFF };

/**
 * @brief A battery test asked through the driver and not yet seen running, whether one runs
 * now, and whether the last one was stopped (test_asked, testing, test_stopped of the C driver).
 */
struct TestState {
    bool asked = false;
    uint32_t asked_ms = 0;
    bool testing = false;
    bool stopped = false;
};

// What a command writes: register, value, and what it is for the driver's bookkeeping
enum class CommandKind : uint8_t { BEEPER, TEST_START, TEST_STOP, INVERTER };
struct CommandWrite {
    uint16_t reg = 0;
    uint16_t value = 0;
    CommandKind kind = CommandKind::BEEPER;
};

// The instant commands of the C driver, in its order
const char* const* commandNames(size_t& count);

/**
 * @brief instcmd() of the C driver without the I/O: the write a command makes. beeper.toggle
 * flips beeper_muted; test.battery.start takes its length in minutes (1-99, 1 when empty).
 *
 * @return OK, NOT_SUPPORTED for a command it does not know, INVALID_ARGUMENT for minutes
 * out of range
 */
CommandResult commandWrite(const char* name, const char* param, bool beeper_muted, CommandWrite& out);

// The mode from the status bits, between them (starting up) from the mode register 0x48
Mode mode(const uint16_t* r, const uint8_t* bit);

/**
 * @brief The variables upsdrv_initinfo() sets once: names, type and ratings of the
 * Prime 1102 XL (2 kVA / 2 kW, 48 V battery).
 */
void setStaticInfo(UPSData& data, int lowbatt = LOWBATT);

/**
 * @brief upsdrv_updateinfo() after a full read: ups.status (OL, OB DISCHRG [LB], OL CHRG,
 * OL BYPASS, OFF) as UPSData flags, and the readings.
 *
 * @param r the 0x50 input registers
 * @param bit the 128 discrete inputs, one per byte
 *
 * With test, the OL CAL branch of the C driver: battery mode with mains present while a test
 * asked through the driver runs is OL CAL, not OB, and ups.test.result follows the test. A
 * test started from the front panel shows as OB, as in the C driver.
 */
void decode(const uint16_t* r, const uint8_t* bit, UPSData& data, int lowbatt = LOWBATT,
            TestState* test = nullptr, uint32_t now_ms = 0);

// After a command the UPS took: ups.test.result and the test bookkeeping, as instcmd()
void noteCommand(const CommandWrite& w, TestState& test, UPSData& data, uint32_t now_ms);

// ups.beeper.status from holding register 0x13
void decodeBeeper(uint16_t value, UPSData& data);

// The ASCII model of holding 0xA1-0xA7, high byte first ("EA900_1-3K_G4"); out holds
// at least 2 x MODEL_REGS + 1 bytes
void decodeModel(const uint16_t* regs, char* out, size_t out_size);

} // namespace Prime

#endif // PRIME_PROTOCOL_H
