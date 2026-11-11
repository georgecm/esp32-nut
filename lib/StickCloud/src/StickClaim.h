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
 * it on bridge.connect.tescom.gr, keep what the cloud hands over. Cloud side: nut-app
 * cloud, POST /v1/stick/claim/start and /v1/stick/claim/poll.
 *
 * Pure logic (no network, no clock of its own), so it runs in the native unit tests;
 * the firmware's cloud task does the HTTPS calls and feeds the answers in.
 */
namespace StickClaim {

// What the cloud hands over once the code is approved (/agent/register's answer)
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

    bool complete() const {
        return !device_id.empty() && !agent_token.empty() && !zabbix_server.empty() && !zabbix_host.empty() &&
               !server_ca_pem.empty() && !cert_pem.empty();
    }
};

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
};
std::string startBody(const StartRequest& r);
std::string pollBody(const std::string& poll_token);
// {"agentVersion":..,"status":{..}} for /v1/agent/heartbeat
std::string heartbeatBody(const std::string& fw_version, uint64_t started_at_ms, const char* link,
                          const char* ups_status);

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
    const std::string& pollToken() const { return _poll_token; }
    const Credentials& credentials() const { return _creds; }
    const std::string& lastError() const { return _error; }
    // Seconds the code still lasts, 0 when none
    uint32_t expiresInS(uint32_t now) const;

    // A stick that was claimed before (credentials from NVS)
    void restore(const Credentials& c);
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
    std::string _error;
    Credentials _creds;
};

} // namespace StickClaim

#endif // STICK_CLAIM_H
