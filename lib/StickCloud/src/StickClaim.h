/*
 * esp32-nut
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STICK_CLAIM_H
#define STICK_CLAIM_H

#include <stdint.h>
#include <string>

/**
 * @brief The stick's side of the Tescom Connect claim (OAuth device flow, RFC 8628):
 * ask the cloud for a code without credentials, show it, poll until the owner approved
 * it in Tescom Connect (connect.tescom.gr/?link-stick=CODE), keep what the cloud hands
 * over. A stick with a factory secret (its QR code) that is already in an account is
 * approved at once. Cloud side: nut-app cloud, POST /v1/stick/claim/start and
 * /v1/stick/claim/poll.
 *
 * The stick is a gateway, not a UPS: the owner adds the UPS behind it by serial in
 * Tescom Connect, and the stick sends readings only once that UPS is on a cloud plan.
 * The Zabbix host comes with the heartbeat answer then, and is null until (and after).
 *
 * Pure logic (no network, no clock of its own), so it runs in the native unit tests;
 * the firmware's cloud task does the HTTPS calls and feeds the answers in.
 */
namespace StickClaim {

// Where readings go, from the poll or heartbeat answer; empty while the cloud says null
struct ZabbixTarget {
    std::string server;
    uint16_t port = 10051;
    std::string host;
    bool empty() const { return server.empty() || host.empty(); }
    bool operator==(const ZabbixTarget& o) const { return server == o.server && port == o.port && host == o.host; }
    bool operator!=(const ZabbixTarget& o) const { return !(*this == o); }
};

// What the cloud hands over once the code is approved
struct Credentials {
    std::string device_id;
    std::string name;
    std::string organization;
    std::string agent_token;
    std::string zabbix_server;
    uint16_t zabbix_port = 10051;
    std::string zabbix_host;
    std::string ca_pem;         // the Connect CA
    std::string server_ca_pem;  // the CA of the Zabbix server's certificate
    std::string cert_pem;       // the stick's certificate, for its own key

    // What the owner still has to do in Tescom Connect: "ups", "plan", or "sending"
    std::string next;
    std::string manage_url;  // where they do it

    ZabbixTarget zabbix() const { return {zabbix_server, zabbix_port, zabbix_host}; }
    void setZabbix(const ZabbixTarget& z) {
        zabbix_server = z.server;
        zabbix_port = z.port;
        zabbix_host = z.host;
    }
    // The Zabbix host may come later (heartbeat), so it is not needed here
    bool complete() const {
        return !device_id.empty() && !agent_token.empty() && !server_ca_pem.empty() && !cert_pem.empty();
    }
};

/**
 * @brief The cloud's answer to the stick's heartbeat.
 * known is false when the answer has no "zabbix" field at all (an older cloud):
 * keep what there is then.
 */
struct HeartbeatAnswer {
    bool known = false;
    ZabbixTarget zabbix;
    std::string next;
};
bool parseHeartbeat(const std::string& body, HeartbeatAnswer& out);

enum class State : uint8_t {
    UNCLAIMED,  // no code yet, or the last one expired: start again
    PENDING,    // a code is shown, polling
    CLAIMED,    // credentials kept; the cloud knows this stick
};
const char* stateName(State s);

struct StartRequest {
    std::string tenant = "tescom";
    std::string mac;         // aa:bb:cc:dd:ee:ff
    std::string serial;      // the UPS's, when it reports one
    std::string model;       // the UPS's, when known
    std::string fw_version;
    std::string csr_pem;
    std::string secret;      // the factory secret (the QR code's), when it has one
};
std::string startBody(const StartRequest& r);
std::string pollBody(const std::string& poll_token);
// {"agentVersion":..,"status":{..}} for /v1/agent/heartbeat
std::string heartbeatBody(const std::string& fw_version, uint64_t started_at_ms, const char* link,
                          const char* ups_status);

/**
 * @brief A shown code, kept across the restart that ends the setup network: the code on
 * the setup page stays the one to approve (a new start would replace it in the cloud).
 */
struct PendingCode {
    std::string user_code;
    std::string verify_url;
    std::string poll_token;
    uint32_t expires_in_s = 0;
    uint32_t interval_s = 5;
};

/**
 * @brief The claim as it moves: what to show, when to call the cloud next.
 *
 * Times are the caller's milliseconds (millis()); unsigned arithmetic, so they wrap safely.
 */
class Flow {
public:
    State state() const { return _state; }
    const std::string& userCode() const { return _user_code; }
    const std::string& verifyUrl() const { return _verify_url; }
    // The cloud approved the stick at once (its factory secret): no code to show
    bool preApproved() const { return _pre_approved; }
    const std::string& pollToken() const { return _poll_token; }
    const Credentials& credentials() const { return _creds; }
    const std::string& lastError() const { return _error; }
    // Seconds the code still lasts, 0 when none
    uint32_t expiresInS(uint32_t now) const;

    // A stick that was claimed before (credentials from NVS)
    void restore(const Credentials& c);
    // The code being shown, to keep across a restart; false when there is none (or the
    // stick was approved at once: nothing to show, a new start approves it again)
    bool pending(PendingCode& out, uint32_t now) const;
    // Go on with a code from before the restart: polled at once; ignored when expired or incomplete
    bool resume(const PendingCode& p, uint32_t now);
    // Forget everything: the device was removed in the cloud, or the owner reset the stick
    void reset();

    // A pending code past its time is dropped, so the next call starts again
    void expireIfNeeded(uint32_t now);

    // Whether the cloud should be called now (start or poll)
    bool due(uint32_t now) const;

    /**
     * @brief The answer to POST /v1/stick/claim/start.
     * @return false when it was not a code (the error is kept; retried after a backoff)
     */
    bool onStart(int http_status, const std::string& body, uint32_t now);

    /**
     * @brief The answer to POST /v1/stick/claim/poll.
     * @return true when the stick was just claimed (credentials() to be saved)
     */
    bool onPoll(int http_status, const std::string& body, uint32_t now);

    // A call that got no HTTP answer at all (no route, DNS, TLS)
    void onNetworkError(const std::string& what, uint32_t now);

private:
    void retryLater(uint32_t now);

    State _state = State::UNCLAIMED;
    std::string _user_code;
    std::string _verify_url;
    std::string _poll_token;
    uint32_t _interval_ms = 5000;
    uint32_t _expires_at = 0;
    uint32_t _next_at = 0;
    bool _has_next = false;
    uint32_t _backoff_ms = 0;
    bool _pre_approved = false;
    std::string _error;
    Credentials _creds;
};

} // namespace StickClaim

#endif // STICK_CLAIM_H
