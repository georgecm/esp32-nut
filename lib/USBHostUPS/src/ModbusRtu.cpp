/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Port of the Modbus RTU master of tescom_prime.c (Tescom Connect, nut-app
 * deploy/nut-drivers/tescom_prime.c, driver version 0.04).
 */

#include "ModbusRtu.h"
#include <string.h>

namespace ModbusRtu {

uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

size_t buildRead(uint8_t slave, uint8_t function, uint16_t start, uint16_t count, uint8_t* out) {
    if (!out || count == 0) return 0;
    if (function == READ_DISCRETE_INPUTS) {
        if (count > MAX_BITS) return 0;
    } else if (function == READ_HOLDING_REGISTERS || function == READ_INPUT_REGISTERS) {
        if (count > MAX_REGISTERS) return 0;
    } else {
        return 0;
    }
    out[0] = slave;
    out[1] = function;
    out[2] = (uint8_t)(start >> 8);
    out[3] = (uint8_t)(start & 0xFF);
    out[4] = (uint8_t)(count >> 8);
    out[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = crc16(out, 6);
    out[6] = (uint8_t)(crc & 0xFF);
    out[7] = (uint8_t)(crc >> 8);
    return REQUEST_LENGTH;
}

size_t buildWrite(uint8_t slave, uint16_t reg, uint16_t value, uint8_t* out) {
    if (!out) return 0;
    out[0] = slave;
    out[1] = WRITE_SINGLE_REGISTER;
    out[2] = (uint8_t)(reg >> 8);
    out[3] = (uint8_t)(reg & 0xFF);
    out[4] = (uint8_t)(value >> 8);
    out[5] = (uint8_t)(value & 0xFF);
    uint16_t crc = crc16(out, 6);
    out[6] = (uint8_t)(crc & 0xFF);
    out[7] = (uint8_t)(crc >> 8);
    return REQUEST_LENGTH;
}

size_t replyLength(uint8_t function, uint16_t count) {
    if (function == READ_DISCRETE_INPUTS) return 5 + (count + 7) / 8;
    return 5 + 2 * (size_t)count;
}

bool checkCrc(const uint8_t* frame, size_t len) {
    if (!frame || len < 4) return false;
    uint16_t crc = crc16(frame, len - 2);
    return frame[len - 2] == (crc & 0xFF) && frame[len - 1] == (crc >> 8);
}

bool decodeRegisters(const uint8_t* reply, size_t len, uint16_t count, uint16_t* out) {
    if (!reply || !out || len < replyLength(READ_INPUT_REGISTERS, count) || reply[2] != 2 * count) return false;
    for (uint16_t i = 0; i < count; i++) {
        out[i] = (uint16_t)((reply[3 + 2 * i] << 8) | reply[4 + 2 * i]);
    }
    return true;
}

bool decodeBits(const uint8_t* reply, size_t len, uint16_t count, uint8_t* out) {
    size_t bytes = (count + 7) / 8;
    if (!reply || !out || len < 5 + bytes || reply[2] != bytes) return false;
    for (uint16_t i = 0; i < count; i++) {
        out[i] = (reply[3 + i / 8] >> (i % 8)) & 1;
    }
    return true;
}

Result Master::transact(const uint8_t* req, uint8_t* reply, size_t expect, size_t& got) {
    got = 0;
    _last_tries = 0;
    for (uint8_t t = 0; t < TRIES; t++) {
        _last_tries = t + 1;
        // "The Prime ignores a request sent right after its last reply"
        _port.sleepMs(REQUEST_GAP_MS);
        _port.flushInput();
        if (!_port.write(req, REQUEST_LENGTH)) continue;

        size_t n = _port.read(reply, 3, BYTE_TIMEOUT_MS);
        if (n != 3 || reply[0] != _slave) {
            _port.sleepMs(RESYNC_PAUSE_MS);
            continue;
        }
        // An exception reply is 5 bytes; a normal one `expect`
        bool exception = (reply[1] & 0x80) != 0;
        size_t rest = exception ? 2 : expect - 3;
        n = _port.read(reply + 3, rest, BYTE_TIMEOUT_MS);
        if (n != rest) continue;
        if (!checkCrc(reply, 3 + rest)) continue;
        // Not in the C driver: a reply to another function is out of step, tried again
        if ((reply[1] & 0x7F) != req[1]) continue;
        if (exception) {
            _last_exception = reply[2];
            return Result::EXCEPTION;
        }
        got = 3 + rest;
        return Result::OK;
    }
    return Result::NO_REPLY;
}

Result Master::readRegisters(uint8_t function, uint16_t start, uint16_t count, uint16_t* out) {
    uint8_t req[REQUEST_LENGTH];
    if (!out || (function != READ_HOLDING_REGISTERS && function != READ_INPUT_REGISTERS) ||
        buildRead(_slave, function, start, count, req) != REQUEST_LENGTH) {
        return Result::BAD_REQUEST;
    }
    uint8_t reply[MAX_REPLY];
    size_t got = 0;
    Result r = transact(req, reply, replyLength(function, count), got);
    if (r != Result::OK) return r;
    // read_regs(): reply[2] != 2 * count is a failed read
    return decodeRegisters(reply, got, count, out) ? Result::OK : Result::NO_REPLY;
}

Result Master::writeRegister(uint16_t reg, uint16_t value) {
    uint8_t req[REQUEST_LENGTH];
    buildWrite(_slave, reg, value, req);
    uint8_t reply[MAX_REPLY];
    size_t got = 0;
    Result r = transact(req, reply, REQUEST_LENGTH, got);
    if (r != Result::OK) return r;
    // write_reg(): the UPS took it when it echoes the request
    return got == REQUEST_LENGTH && memcmp(reply, req, REQUEST_LENGTH) == 0 ? Result::OK : Result::NO_REPLY;
}

Result Master::readBits(uint16_t start, uint16_t count, uint8_t* out) {
    uint8_t req[REQUEST_LENGTH];
    if (!out || buildRead(_slave, READ_DISCRETE_INPUTS, start, count, req) != REQUEST_LENGTH) {
        return Result::BAD_REQUEST;
    }
    uint8_t reply[MAX_REPLY];
    size_t got = 0;
    Result r = transact(req, reply, replyLength(READ_DISCRETE_INPUTS, count), got);
    if (r != Result::OK) return r;
    return decodeBits(reply, got, count, out) ? Result::OK : Result::NO_REPLY;
}

} // namespace ModbusRtu
