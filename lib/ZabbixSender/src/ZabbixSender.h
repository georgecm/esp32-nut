/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZABBIX_SENDER_H
#define ZABBIX_SENDER_H

#include <stddef.h>
#include <stdint.h>
#include <deque>
#include <string>
#include <vector>

class UPSData;

/**
 * @brief The Zabbix sender (trapper) protocol and the UPS items Tescom's Zabbix template
 * expects, as tescom-agent sends them (nut-app crates/tescom-agent: items.rs, zabbix.rs).
 *
 * Pure logic, so it runs in the native unit tests; the socket is the caller's.
 */
namespace Zabbix {

struct Item {
    std::string host;
    std::string key;
    std::string value;
    uint64_t clock; // Unix seconds; 0 = unknown (no time yet): the server stamps it
};

// ups.link of the template: the state of the link between the board and the UPS
enum class Link { OK, STALE, DRIVER_DOWN };
const char* linkName(Link link);

/**
 * @brief The template's items for one reading (items.rs): ups.link always; the rest only
 * when the link is OK, so on a stale or lost link Zabbix keeps the last real values and
 * its nodata triggers fire. Numbers not reported are not sent.
 *
 * @param status the NUT ups.status string ("OL CHRG"), as the NUT server reports it
 */
void items(const char* host, Link link, const char* status, const UPSData* data, uint64_t clock,
           std::vector<Item>& out);

// A number as Rust's f64 Display writes it (the agent's format): 26.1, 0, 100, 1.56
std::string formatNumber(double v);

// {"request":"sender data","data":[...],"clock":N}; clocks of 0 are left out
std::string requestJson(const std::vector<Item>& items, uint64_t clock);

// "ZBXD\x01", the JSON length as 64-bit little-endian, the JSON
std::string frame(const std::string& json);

struct Outcome {
    bool ok = false;           // the server answered "success"
    uint32_t processed = 0;
    uint32_t failed = 0;
    uint32_t total = 0;
    std::string info;          // the server's info text, or why the reply is not one
};

/**
 * @brief Parses a complete reply (header and JSON). Fills processed/failed/total from
 * "processed: 3; failed: 0; total: 3; seconds spent: 0.000".
 *
 * @return false when the bytes are not a Zabbix reply
 */
bool parseReply(const uint8_t* data, size_t len, Outcome& out);

// How many bytes the reply has in all, once the 13-byte header is in; 0 if not valid
size_t replyLength(const uint8_t* header, size_t len);

/**
 * @brief Values waiting while Zabbix cannot be reached (the agent's backlog): the oldest
 * go first when it is full.
 */
class Backlog {
public:
    explicit Backlog(size_t max_items = 2000) : _max(max_items) {}
    void add(const std::vector<Item>& items);
    const std::deque<Item>& items() const { return _items; }
    size_t size() const { return _items.size(); }
    size_t dropped() const { return _dropped; }
    void clear() { _items.clear(); }

private:
    size_t _max;
    size_t _dropped = 0;
    std::deque<Item> _items;
};

} // namespace Zabbix

#endif // ZABBIX_SENDER_H
