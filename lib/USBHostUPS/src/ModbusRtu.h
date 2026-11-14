/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Port of the Modbus RTU master of tescom_prime.c (Tescom Connect, nut-app
 * deploy/nut-drivers/tescom_prime.c, driver version 0.04).
 */

#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <stddef.h>
#include <stdint.h>
#include "ISerialPort.h"

namespace ModbusRtu {

static const uint8_t READ_DISCRETE_INPUTS = 0x02;
static const uint8_t READ_HOLDING_REGISTERS = 0x03;
static const uint8_t READ_INPUT_REGISTERS = 0x04;

// Longest read of one request: registers (read_regs) and bits (read_bits, 8 bytes)
static const uint16_t MAX_REGISTERS = 32;
static const uint16_t MAX_BITS = 64;
static const size_t REQUEST_LENGTH = 8;
static const size_t MAX_REPLY = 5 + 2 * MAX_REGISTERS;

enum class Result : uint8_t {
    OK,
    NO_REPLY,   // nothing valid came back after every try (timeouts, bad CRC, wrong slave)
    EXCEPTION,  // the slave answered with function | 0x80: not retried, as the C driver
    BAD_REQUEST,
};

// CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF); sent low byte first
uint16_t crc16(const uint8_t* data, size_t len);

/**
 * @brief The 8-byte request of a read (functions 02, 03, 04): slave, function, start
 * (big-endian), count (big-endian), CRC (little-endian).
 *
 * @return REQUEST_LENGTH, or 0 for a count the C driver would refuse
 */
size_t buildRead(uint8_t slave, uint8_t function, uint16_t start, uint16_t count, uint8_t* out);

// Length of the normal reply to a read: 5 + byte count
size_t replyLength(uint8_t function, uint16_t count);

// True when the CRC at the end of a frame of len bytes is right
bool checkCrc(const uint8_t* frame, size_t len);

// The registers of a normal reply (big-endian pairs after the byte count); false when its
// byte count is not 2 x count
bool decodeRegisters(const uint8_t* reply, size_t len, uint16_t count, uint16_t* out);
// The bits of a normal reply to function 02, one per byte, LSB of each data byte first
bool decodeBits(const uint8_t* reply, size_t len, uint16_t count, uint8_t* out);

/**
 * @brief transact() of tescom_prime.c over an ISerialPort: each request is tried TRIES
 * times, with REQUEST_GAP_MS before each (the Prime ignores a request sent right after its
 * last reply) and the input flushed; the reply is read as 3 bytes, then the rest (2 for an
 * exception), each within BYTE_TIMEOUT_MS; a wrong slave, a function that is not the one
 * asked, a short frame or a bad CRC is another try.
 */
class Master {
public:
    static const uint8_t TRIES = 3;
    static const uint32_t REQUEST_GAP_MS = 50;
    static const uint32_t BYTE_TIMEOUT_MS = 800;
    // Pause after a reply that did not start with our slave address (usleep(50000))
    static const uint32_t RESYNC_PAUSE_MS = 50;

    explicit Master(ISerialPort& port, uint8_t slave = 1) : _port(port), _slave(slave) {}

    // Function 03 or 04: count (1-32) registers from start into out
    Result readRegisters(uint8_t function, uint16_t start, uint16_t count, uint16_t* out);
    // Function 02: count (1-64) discrete inputs from start into out, one per byte
    Result readBits(uint16_t start, uint16_t count, uint8_t* out);

    // Exception code of the last EXCEPTION result
    uint8_t lastException() const { return _last_exception; }
    // Tries the last request took (1 when it was answered at once)
    uint8_t lastTries() const { return _last_tries; }

private:
    Result transact(const uint8_t* req, uint8_t* reply, size_t expect, size_t& got);

    ISerialPort& _port;
    uint8_t _slave;
    uint8_t _last_exception = 0;
    uint8_t _last_tries = 0;
};

} // namespace ModbusRtu

#endif // MODBUS_RTU_H
