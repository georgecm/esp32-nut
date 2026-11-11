/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ZabbixSender.h"
#include "UPSData.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

namespace Zabbix {

namespace {

const char HEADER[] = "ZBXD\x01";
const size_t HEADER_LEN = 5;
const size_t PREFIX_LEN = HEADER_LEN + 8;
// Replies are a short JSON object; anything bigger is not a Zabbix server (zabbix.rs)
const uint64_t MAX_REPLY = 64 * 1024;

bool hasFlag(const char* status, const char* flag) {
    size_t n = strlen(flag);
    for (const char* p = status; p && *p;) {
        while (*p == ' ') p++;
        const char* end = p;
        while (*end && *end != ' ') end++;
        if ((size_t)(end - p) == n && strncmp(p, flag, n) == 0) return true;
        p = end;
    }
    return false;
}

const char* powerName(const char* status) {
    if (hasFlag(status, "OFF")) return "off";
    if (hasFlag(status, "BYPASS")) return "bypass";
    if (hasFlag(status, "OB")) return "battery";
    if (hasFlag(status, "OL")) return "mains";
    return "unknown";
}

std::string trim(const char* s) {
    std::string out = s ? s : "";
    size_t a = out.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = out.find_last_not_of(" \t\r\n");
    return out.substr(a, b - a + 1);
}

bool parseNumber(const std::string& s, double& out) {
    std::string t = trim(s.c_str());
    if (t.empty()) return false;
    char* end = nullptr;
    out = strtod(t.c_str(), &end);
    return end && *end == 0 && isfinite(out);
}

void jsonString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
}

uint32_t infoValue(const std::string& info, const char* name) {
    size_t at = info.find(name);
    if (at == std::string::npos) return 0;
    at = info.find(':', at);
    if (at == std::string::npos) return 0;
    return (uint32_t)strtoul(info.c_str() + at + 1, nullptr, 10);
}

// The string value of "key" in a flat JSON object, unescaped enough for Zabbix replies
bool jsonField(const std::string& json, const char* key, std::string& out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t at = json.find(pat);
    if (at == std::string::npos) return false;
    at = json.find(':', at + pat.size());
    if (at == std::string::npos) return false;
    at = json.find('"', at);
    if (at == std::string::npos) return false;
    out.clear();
    for (size_t i = at + 1; i < json.size(); i++) {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            out += json[++i];
        } else if (c == '"') {
            return true;
        } else {
            out += c;
        }
    }
    return false;
}

} // namespace

const char* linkName(Link link) {
    switch (link) {
    case Link::OK: return "ok";
    case Link::STALE: return "stale";
    case Link::DRIVER_DOWN: return "driverDown";
    }
    return "error";
}

std::string formatNumber(double v) {
    if (v == 0) return "0"; // also -0
    char buf[40];
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (strtod(buf, nullptr) == v) break;
    }
    if (!strchr(buf, 'e')) return buf;
    // Rust writes no exponent in this range; UPS values never reach it
    snprintf(buf, sizeof(buf), "%.6f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

void items(const char* host, Link link, const char* status, const UPSData* data, uint64_t clock,
           std::vector<Item>& out) {
    auto push = [&](const char* key, const std::string& value) { out.push_back({ host, key, value, clock }); };

    push("ups.link", linkName(link));
    if (link != Link::OK) return;

    std::string raw = trim(status);
    push("ups.status", raw);
    push("ups.power", powerName(raw.c_str()));
    push("ups.on_battery", strcmp(powerName(raw.c_str()), "battery") == 0 ? "1" : "0");
    push("ups.low_battery", hasFlag(raw.c_str(), "LB") ? "1" : "0");
    push("ups.replace_battery", hasFlag(raw.c_str(), "RB") ? "1" : "0");
    push("ups.overloaded", hasFlag(raw.c_str(), "OVER") ? "1" : "0");

    if (!data) return;
    auto text = [&](const char* var, std::string& value) {
        const String* v = data->find(var);
        if (!v) return false;
        value = v->c_str();
        return true;
    };
    static const char* const NUMBERS[][2] = {
        { "ups.battery.charge", "battery.charge" },
        { "ups.battery.voltage", "battery.voltage" },
        { "ups.battery.runtime", "battery.runtime" },
        { "ups.load", "ups.load" },
        { "ups.input.voltage", "input.voltage" },
        { "ups.input.frequency", "input.frequency" },
        { "ups.output.voltage", "output.voltage" },
        { "ups.temperature", "ups.temperature" },
    };
    for (const auto& n : NUMBERS) {
        std::string value;
        double v;
        if (text(n[1], value) && parseNumber(value, v)) push(n[0], formatNumber(v));
    }
    std::string value;
    if (text("device.model", value) || text("ups.model", value)) push("ups.model", value);
    if (text("device.serial", value) || text("ups.serial", value)) push("ups.serial", value);
    if (text("driver.name", value)) push("ups.driver", value);
}

std::string requestJson(const std::vector<Item>& items, uint64_t clock) {
    std::string out = "{\"request\":\"sender data\",\"data\":[";
    bool first = true;
    for (const auto& i : items) {
        if (!first) out += ',';
        first = false;
        out += "{\"host\":";
        jsonString(out, i.host);
        out += ",\"key\":";
        jsonString(out, i.key);
        out += ",\"value\":";
        jsonString(out, i.value);
        if (i.clock) out += ",\"clock\":" + std::to_string(i.clock);
        out += '}';
    }
    out += ']';
    if (clock) out += ",\"clock\":" + std::to_string(clock);
    out += '}';
    return out;
}

std::string frame(const std::string& json) {
    std::string out(HEADER, HEADER_LEN);
    uint64_t n = json.size();
    for (int i = 0; i < 8; i++) out += (char)((n >> (8 * i)) & 0xff);
    out += json;
    return out;
}

size_t replyLength(const uint8_t* header, size_t len) {
    if (len < PREFIX_LEN || memcmp(header, HEADER, HEADER_LEN) != 0) return 0;
    uint64_t n = 0;
    for (int i = 7; i >= 0; i--) n = (n << 8) | header[HEADER_LEN + i];
    if (n == 0 || n > MAX_REPLY) return 0;
    return PREFIX_LEN + (size_t)n;
}

bool parseReply(const uint8_t* data, size_t len, Outcome& out) {
    out = Outcome();
    size_t total = replyLength(data, len);
    if (!total || len < total) {
        out.info = "not a Zabbix reply";
        return false;
    }
    std::string json((const char*)data + PREFIX_LEN, total - PREFIX_LEN);
    std::string response;
    if (!jsonField(json, "response", response)) {
        out.info = "reply without response";
        return false;
    }
    jsonField(json, "info", out.info);
    out.ok = response == "success";
    out.processed = infoValue(out.info, "processed");
    out.failed = infoValue(out.info, "failed");
    out.total = infoValue(out.info, "total");
    return true;
}

const char* validate(const Settings& s) {
    if (s.server.size() > 253) return "server: too long";
    for (char c : s.server) {
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':' || c == '_'))
            return "server: a host name or IP address, without spaces";
    }
    // Zabbix host names: letters, digits, spaces, dots, dashes and underscores, 128 at most
    if (s.host.size() > 128) return "host: 128 characters at most";
    for (char c : s.host) {
        if (!(isalnum((unsigned char)c) || c == '.' || c == ' ' || c == '_' || c == '-'))
            return "host: only letters, digits, spaces, dots, dashes and underscores";
    }
    if (!s.host.empty() && (s.host.front() == ' ' || s.host.back() == ' ')) return "host: no space at either end";
    if (s.port == 0) return "port: 1-65535";
    if (s.interval_s < 2 || s.interval_s > 3600) return "interval: 2-3600 seconds";
    if (s.enabled && s.server.empty()) return "server: required";
    if (s.enabled && s.host.empty()) return "host: required";
    return nullptr;
}

void Backlog::add(const std::vector<Item>& items) {
    for (const auto& i : items) _items.push_back(i);
    while (_items.size() > _capacity) {
        _items.pop_front();
        _dropped++;
    }
}

} // namespace Zabbix
