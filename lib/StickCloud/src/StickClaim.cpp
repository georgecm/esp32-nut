/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "StickClaim.h"
#include <ArduinoJson.h>

namespace StickClaim {

namespace {

const uint32_t BACKOFF_MIN_MS = 10000;
const uint32_t BACKOFF_MAX_MS = 300000;

std::string field(JsonVariantConst v) {
    const char* s = v.as<const char*>();
    return s ? s : "";
}

std::string errorOf(int status, const std::string& body) {
    JsonDocument doc;
    if (!deserializeJson(doc, body) && doc["error"].is<const char*>()) return doc["error"].as<const char*>();
    return "HTTP " + std::to_string(status);
}

} // namespace

const char* stateName(State s) {
    switch (s) {
    case State::UNCLAIMED: return "unclaimed";
    case State::PENDING: return "pending";
    case State::CLAIMED: return "claimed";
    }
    return "unknown";
}

std::string startBody(const StartRequest& r) {
    JsonDocument doc;
    doc["tenant"] = r.tenant;
    doc["mac"] = r.mac;
    if (!r.serial.empty()) doc["serial"] = r.serial;
    if (!r.model.empty()) doc["model"] = r.model;
    if (!r.fw_version.empty()) doc["fwVersion"] = r.fw_version;
    doc["csr"] = r.csr_pem;
    if (!r.secret.empty()) doc["secret"] = r.secret;
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string pollBody(const std::string& poll_token) {
    JsonDocument doc;
    doc["pollToken"] = poll_token;
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string heartbeatBody(const std::string& fw_version, uint64_t started_at_ms, const char* link,
                          const char* ups_status) {
    JsonDocument doc;
    doc["agentVersion"] = fw_version;
    JsonObject st = doc["status"].to<JsonObject>();
    if (started_at_ms) st["startedAtMs"] = started_at_ms;
    st["link"] = link ? link : "";
    if (ups_status && *ups_status) st["upsStatus"] = ups_status;
    st["agentKind"] = "stick";
    std::string out;
    serializeJson(doc, out);
    return out;
}

namespace {
ZabbixTarget zabbixOf(JsonVariantConst z) {
    ZabbixTarget t;
    if (!z.is<JsonObjectConst>()) return t;
    t.server = field(z["server"]);
    t.port = z["port"] | 10051;
    t.host = field(z["host"]);
    return t;
}
} // namespace

bool parseHeartbeat(const std::string& body, HeartbeatAnswer& out) {
    out = HeartbeatAnswer();
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    out.next = field(doc["next"]);
    if (doc["zabbix"].isUnbound()) return true;
    out.known = true;
    out.zabbix = zabbixOf(doc["zabbix"]);
    return true;
}

uint32_t Flow::expiresInS(uint32_t now) const {
    if (_state != State::PENDING) return 0;
    int32_t left = (int32_t)(_expires_at - now);
    return left > 0 ? (uint32_t)left / 1000 : 0;
}

void Flow::restore(const Credentials& c) {
    reset();
    _creds = c;
    _state = State::CLAIMED;
}

void Flow::reset() {
    *this = Flow();
}

void Flow::expireIfNeeded(uint32_t now) {
    if (_state == State::PENDING && (int32_t)(now - _expires_at) >= 0) {
        reset();
        _error = "the code expired";
    }
}

bool Flow::due(uint32_t now) const {
    if (_state == State::CLAIMED) return false;
    if (!_has_next) return true;
    return (int32_t)(now - _next_at) >= 0;
}

void Flow::retryLater(uint32_t now) {
    _backoff_ms = _backoff_ms ? _backoff_ms * 2 : BACKOFF_MIN_MS;
    if (_backoff_ms > BACKOFF_MAX_MS) _backoff_ms = BACKOFF_MAX_MS;
    _next_at = now + _backoff_ms;
    _has_next = true;
}

bool Flow::onStart(int http_status, const std::string& body, uint32_t now) {
    if (http_status != 201 && http_status != 200) {
        _error = errorOf(http_status, body);
        _state = State::UNCLAIMED;
        retryLater(now);
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc["userCode"].is<const char*>() || !doc["pollToken"].is<const char*>()) {
        _error = "the cloud's answer is not a code";
        _state = State::UNCLAIMED;
        retryLater(now);
        return false;
    }
    _user_code = field(doc["userCode"]);
    _verify_url = field(doc["verifyUrl"]);
    _poll_token = field(doc["pollToken"]);
    uint32_t interval_s = doc["interval"] | 5;
    if (interval_s < 2) interval_s = 2;  // the cloud's own 1 s is only for a pre-approved stick
    uint32_t ttl_s = doc["expiresIn"] | 900;
    _pre_approved = doc["approved"] | false;
    if (_pre_approved) interval_s = 1;
    _interval_ms = interval_s * 1000;
    _expires_at = now + ttl_s * 1000;
    _next_at = now + _interval_ms;
    _has_next = true;
    _backoff_ms = 0;
    _error.clear();
    _state = State::PENDING;
    return true;
}

bool Flow::onPoll(int http_status, const std::string& body, uint32_t now) {
    if (_state != State::PENDING) return false;
    if (http_status == 202) {
        // Still waiting; the cloud may say how long the code lasts now
        JsonDocument doc;
        if (!deserializeJson(doc, body) && doc["expiresIn"].is<uint32_t>()) {
            _expires_at = now + doc["expiresIn"].as<uint32_t>() * 1000;
        }
        _next_at = now + _interval_ms;
        _has_next = true;
        _error.clear();
        return false;
    }
    if (http_status == 404 || http_status == 410) {
        // Expired, replaced or delivered elsewhere: a new code at once
        _error = errorOf(http_status, body);
        std::string error = _error;
        reset();
        _error = error;
        return false;
    }
    if (http_status != 200) {
        _error = errorOf(http_status, body);
        _next_at = now + _interval_ms * 2;
        _has_next = true;
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        _error = "the cloud's answer could not be read";
        _next_at = now + _interval_ms;
        _has_next = true;
        return false;
    }
    Credentials c;
    c.device_id = field(doc["deviceId"]);
    c.name = field(doc["name"]);
    c.organization = field(doc["organization"]);
    c.agent_token = field(doc["agentToken"]);
    c.setZabbix(zabbixOf(doc["zabbix"]));
    c.next = field(doc["next"]);
    c.manage_url = field(doc["manageUrl"]);
    c.ca_pem = field(doc["tls"]["ca"]);
    c.server_ca_pem = field(doc["tls"]["serverCa"]);
    c.cert_pem = field(doc["tls"]["cert"]);
    if (!c.complete()) {
        _error = "the cloud's answer is missing credentials";
        _next_at = now + _interval_ms;
        _has_next = true;
        return false;
    }
    restore(c);
    return true;
}

void Flow::onNetworkError(const std::string& what, uint32_t now) {
    _error = what;
    retryLater(now);
}

} // namespace StickClaim
