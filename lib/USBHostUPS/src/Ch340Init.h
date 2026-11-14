/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The CH340/CH341 vendor requests as Linux drivers/usb/serial/ch341.c sends them
 * (ch341_configure, ch341_get_divisor, ch341_set_baudrate_lcr, ch341_set_handshake),
 * GPL-2.0, Copyright 2007 Frank A Kingswood and the Linux contributors. Rewritten here.
 */

#ifndef CH340_INIT_H
#define CH340_INIT_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Pure logic of the CH340 set-up, no USB: what Ch340Host sends, testable natively.
 *
 * All requests are vendor requests to the device (bmRequestType 0x40 out, 0xC0 in).
 */
namespace Ch340 {

static const uint8_t REQUEST_OUT = 0x40; // USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_OUT
static const uint8_t REQUEST_IN = 0xC0;

static const uint8_t REQ_READ_VERSION = 0x5F;
static const uint8_t REQ_WRITE_REG = 0x9A;
static const uint8_t REQ_READ_REG = 0x95;
static const uint8_t REQ_SERIAL_INIT = 0xA1;
static const uint8_t REQ_MODEM_CTRL = 0xA4;

static const uint8_t REG_PRESCALER = 0x12;
static const uint8_t REG_DIVISOR = 0x13;
static const uint8_t REG_LCR = 0x18;
static const uint8_t REG_LCR2 = 0x25;

static const uint8_t LCR_ENABLE_RX = 0x80;
static const uint8_t LCR_ENABLE_TX = 0x40;
static const uint8_t LCR_CS8 = 0x03;
// 8 data bits, no parity, 1 stop bit, receiver and transmitter on
static const uint8_t LCR_8N1 = LCR_ENABLE_RX | LCR_ENABLE_TX | LCR_CS8;

static const uint8_t BIT_DTR = 1 << 5;
static const uint8_t BIT_RTS = 1 << 6;

static const uint32_t CLKRATE = 48000000;

struct Request {
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
};

/**
 * @brief ch341_get_divisor(): (0x100 - div) << 8 | fact << 2 | ps, -1 for a rate out of
 * range. limited_prescaler is CH341_QUIRK_LIMITED_PRESCALER (some chips); 9600 comes out
 * the same either way (0xB202).
 */
inline int32_t divisor(uint32_t speed, bool limited_prescaler = false) {
    if (speed < 46) speed = 46;
    if (speed > 3000000) speed = 3000000;
    // CH341_CLK_DIV(ps, fact) = 1 << (12 - 3 * ps - fact); CH341_MIN_RATE(ps) with fact 1
    auto clkDiv = [](int ps, int fact) -> uint32_t { return 1u << (12 - 3 * ps - fact); };
    auto minRate = [&](int ps) -> uint32_t { return CLKRATE / (clkDiv(ps, 1) * 512); };

    uint32_t fact = 1;
    int ps;
    for (ps = 3; ps >= 0; ps--) {
        if (speed > minRate(ps)) break;
    }
    if (ps < 0) return -1;
    uint32_t clk_div = clkDiv(ps, (int)fact);
    uint32_t div = CLKRATE / (clk_div * speed);
    bool force_fact0 = ps < 3 && limited_prescaler;
    if (div < 9 || div > 255 || force_fact0) {
        div /= 2;
        clk_div *= 2;
        fact = 0;
    }
    if (div < 2) return -1;
    // The next divisor when it gives a rate closer to the one asked
    if (16 * CLKRATE / (clk_div * div) - 16 * speed >= 16 * speed - 16 * CLKRATE / (clk_div * (div + 1))) div++;
    // The lower base clock when the divisor is even
    if (fact == 1 && div % 2 == 0) {
        div /= 2;
        fact = 0;
    }
    return (int32_t)(((0x100 - div) << 8) | (fact << 2) | (uint32_t)ps);
}

/**
 * @brief The requests of ch341_configure() after the version read (REQ_READ_VERSION,
 * 2 bytes): serial init, divisor (bit 7 set on chips after version 0x27, or the chip
 * buffers until 32 bytes have come), line control (chips from version 0x30 only), then the
 * modem lines (REQ_MODEM_CTRL takes them inverted).
 *
 * @return how many requests were written to out (at most 4), 0 for a rate out of range
 */
inline size_t serialInit(uint8_t version, uint32_t baud, uint8_t lcr, uint8_t mcr, Request* out, size_t max) {
    int32_t div = divisor(baud);
    if (div < 0 || !out || max < 4) return 0;
    size_t n = 0;
    out[n++] = { REQ_SERIAL_INIT, 0, 0 };
    uint16_t val = (uint16_t)div;
    if (version > 0x27) val |= 1u << 7;
    out[n++] = { REQ_WRITE_REG, (uint16_t)(REG_DIVISOR << 8 | REG_PRESCALER), val };
    if (version >= 0x30) out[n++] = { REQ_WRITE_REG, (uint16_t)(REG_LCR2 << 8 | REG_LCR), lcr };
    out[n++] = { REQ_MODEM_CTRL, (uint16_t)~mcr, 0 };
    return n;
}

} // namespace Ch340

#endif // CH340_INIT_H
