/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef I_SERIAL_PORT_H
#define I_SERIAL_PORT_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief A serial line as a serial UPS driver sees it: the CH340 USB-serial bridge of the
 * Tescom Prime on the board (Ch340Host behind USBHostUPS), a scripted UPS in the native tests.
 *
 * The line is already set up (9600 8N1 for the Prime) when the driver gets it.
 */
class ISerialPort {
public:
    virtual ~ISerialPort() = default;

    // Sends every byte; false when the transfer failed or timed out
    virtual bool write(const uint8_t* data, size_t len) = 0;
    /**
     * @brief Reads up to len bytes, waiting at most timeout_ms for all of them (ser_get_buf_len
     * of NUT). Bytes that arrive past len stay for the next read.
     *
     * @return how many bytes were read: less than len on a timeout
     */
    virtual size_t read(uint8_t* buf, size_t len, uint32_t timeout_ms) = 0;
    // Drops whatever has been received and not read yet (ser_flush_in)
    virtual void flushInput() = 0;
    // Pause between requests (usleep in the NUT driver)
    virtual void sleepMs(uint32_t ms) = 0;
    /**
     * @brief The device behind the line answered a request (true), or still did not after its
     * retries (false). The host's link monitor, as the control pipe results of the HID path:
     * a silent UPS turns the data stale.
     */
    virtual void noteExchange(bool answered) {}
};

#endif // I_SERIAL_PORT_H
