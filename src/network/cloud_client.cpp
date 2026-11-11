#include "network/cloud_client.h"
#include "USBHostUPS.h"
#include "core/app_logger.h"
#include "network/network_manager.h"
#include "network/stick_identity.h"
#include "network/tls_gate.h"
#include "network/zabbix_client.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <time.h>

#ifndef CLOUD_URL
#define CLOUD_URL "https://bridge.connect.tescom.gr"
#endif
#ifndef CLOUD_TENANT
#define CLOUD_TENANT "tescom"
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

namespace {

const char* NVS_NAMESPACE = "cloud";
const uint32_t HEARTBEAT_MS = 30000;
const int HTTP_TIMEOUT_MS = 15000;
// Answers are small JSON; the claim's credentials are the largest (two PEMs, a JWT)
const size_t MAX_BODY = 16 * 1024;

class Guard {
public:
    explicit Guard(SemaphoreHandle_t m) : _m(m) { xSemaphoreTake(_m, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(_m); }

private:
    SemaphoreHandle_t _m;
};

/**
 * @brief POST JSON over HTTPS (ESP-IDF certificate bundle), the answer's body in out.
 * @return the HTTP status, or -1 when there was no answer (DNS, TCP, TLS)
 */
int httpsPost(const String& url, const std::string& body, const std::string& bearer, std::string& out,
              String& error) {
    TlsGate gate;
    out.clear();
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        error = "HTTP client init failed";
        return -1;
    }
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "User-Agent", "esp32-nut-stick/" FIRMWARE_VERSION);
    std::string auth;
    if (!bearer.empty()) {
        auth = "Bearer " + bearer;
        esp_http_client_set_header(h, "Authorization", auth.c_str());
    }
    int status = -1;
    esp_err_t err = esp_http_client_open(h, body.size());
    if (err == ESP_OK && esp_http_client_write(h, body.data(), body.size()) == (int)body.size()) {
        esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        char buf[512];
        int n;
        while ((n = esp_http_client_read(h, buf, sizeof(buf))) > 0) {
            if (out.size() + n > MAX_BODY) break;
            out.append(buf, n);
        }
    } else {
        error = String("no answer from the cloud (") + esp_err_to_name(err) + ")";
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status;
}

} // namespace

void CloudClient::begin(USBHostUPS* ups, AppNetworkManager* net, ZabbixClient* zabbix) {
    _ups = ups;
    _net = net;
    _zabbix = zabbix;
    _mutex = xSemaphoreCreateMutex();
    StickClaim::Credentials c;
    if (loadCredentials(c)) {
        _flow.restore(c);
        applyCredentials(c);
        AppLogger::log("INFO", "[CLOUD] Linked to Tescom Connect as \"%s\" (%s)", c.name.c_str(), c.organization.c_str());
    } else {
        AppLogger::log("INFO", "[CLOUD] Not linked to Tescom Connect yet: a code is asked for once on Wi-Fi");
    }
    xTaskCreatePinnedToCore(taskEntry, "cloud", 8192, this, 1, nullptr, 0);
}

String CloudClient::mac() const {
    uint8_t m[6];
    WiFi.macAddress(m);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return buf;
}

String CloudClient::statusJson() const {
    JsonDocument doc;
    {
        Guard g(_mutex);
        uint32_t now = millis();
        doc["state"] = StickClaim::stateName(_flow.state());
        doc["mac"] = mac();
        if (_flow.state() == StickClaim::State::PENDING) {
            doc["userCode"] = _flow.userCode();
            doc["verifyUrl"] = _flow.verifyUrl();
            doc["expiresIn"] = _flow.expiresInS(now);
        }
        if (_flow.state() == StickClaim::State::CLAIMED) {
            const auto& c = _flow.credentials();
            doc["deviceId"] = c.device_id;
            doc["name"] = c.name;
            doc["organization"] = c.organization;
            doc["heartbeatOk"] = _heartbeat_sent && _heartbeat_error.length() == 0;
            if (_heartbeat_sent && _heartbeat_error.length() == 0) doc["lastHeartbeatAgoS"] = (now - _heartbeat_ok_ms) / 1000;
            if (_heartbeat_error.length()) doc["lastError"] = _heartbeat_error;
        } else if (_flow.lastError().size()) {
            doc["lastError"] = _flow.lastError();
        }
        doc["connected"] = _net && _net->isConnected();
    }
    String out;
    serializeJson(doc, out);
    return out;
}

void CloudClient::unlink() {
    _unlink = true;
}

void CloudClient::taskEntry(void* arg) {
    static_cast<CloudClient*>(arg)->run();
}

void CloudClient::run() {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (_unlink) {
            _unlink = false;
            eraseCredentials();
            if (_zabbix) _zabbix->clearManaged();
            Guard g(_mutex);
            _flow.reset();
            _heartbeat_sent = false;
            _heartbeat_error = "";
            AppLogger::log("INFO", "[CLOUD] Unlinked: a new code will be asked for");
        }
        if (!_net || !_net->isConnected()) continue;
        if (!_started_at_ms) {
            time_t t = time(nullptr);
            if (t > 1700000000) _started_at_ms = (uint64_t)t * 1000 - millis();
        }
        uint32_t now = millis();
        StickClaim::State state;
        {
            Guard g(_mutex);
            _flow.expireIfNeeded(now);
            state = _flow.state();
        }
        if (state == StickClaim::State::CLAIMED) {
            if (!_heartbeat_sent || now - _last_heartbeat >= HEARTBEAT_MS) heartbeat();
        } else {
            claimStep(now);
        }
    }
}

void CloudClient::claimStep(uint32_t now) {
    bool due;
    StickClaim::State state;
    std::string poll_token;
    {
        Guard g(_mutex);
        due = _flow.due(now);
        state = _flow.state();
        poll_token = _flow.pollToken();
    }
    if (!due) return;

    std::string out;
    String error;
    if (state == StickClaim::State::UNCLAIMED) {
        StickClaim::StartRequest req;
        req.tenant = CLOUD_TENANT;
        req.mac = mac().c_str();
        req.fw_version = FIRMWARE_VERSION;
        req.csr_pem = StickIdentity::csrPem(mac());
        {
            auto data = _ups->getUPSData();
            req.model = data->get("ups.model").c_str();
            req.serial = data->get("ups.serial").c_str();
        }
        if (req.csr_pem.empty()) {
            Guard g(_mutex);
            _flow.onNetworkError("could not make the certificate request", now);
            return;
        }
        int status = httpsPost(String(CLOUD_URL) + "/v1/stick/claim/start", StickClaim::startBody(req), "", out, error);
        Guard g(_mutex);
        if (status < 0) {
            _flow.onNetworkError(error.c_str(), millis());
        } else if (_flow.onStart(status, out, millis())) {
            AppLogger::log("INFO", "[CLOUD] Code %s: approve it at %s", _flow.userCode().c_str(), _flow.verifyUrl().c_str());
        } else {
            AppLogger::log("WARN", "[CLOUD] No code from the cloud: %s", _flow.lastError().c_str());
        }
        return;
    }

    int status = httpsPost(String(CLOUD_URL) + "/v1/stick/claim/poll", StickClaim::pollBody(poll_token), "", out, error);
    StickClaim::Credentials claimed;
    bool just_claimed;
    {
        Guard g(_mutex);
        if (status < 0) {
            _flow.onNetworkError(error.c_str(), millis());
            return;
        }
        just_claimed = _flow.onPoll(status, out, millis());
        if (just_claimed) claimed = _flow.credentials();
    }
    if (!just_claimed) return;
    if (!saveCredentials(claimed)) AppLogger::log("ERROR", "[CLOUD] Linked, but the credentials could not be saved");
    applyCredentials(claimed);
    AppLogger::log("INFO", "[CLOUD] Linked to Tescom Connect as \"%s\" (%s)", claimed.name.c_str(), claimed.organization.c_str());
    _heartbeat_sent = false;
}

void CloudClient::heartbeat() {
    std::string token;
    {
        Guard g(_mutex);
        token = _flow.credentials().agent_token;
    }
    const char* link = !_ups->isConnected() ? "driverDown" : _ups->isDataStale() ? "stale" : "ok";
    String status = _ups->getUPSStatusString();
    std::string out;
    String error;
    int code = httpsPost(String(CLOUD_URL) + "/v1/agent/heartbeat",
                         StickClaim::heartbeatBody(FIRMWARE_VERSION, _started_at_ms, link, status.c_str()), token, out, error);
    _last_heartbeat = millis();
    _heartbeat_sent = true;
    if (code == 200) {
        Guard g(_mutex);
        if (_heartbeat_error.length()) AppLogger::log("INFO", "[CLOUD] Heartbeat works again");
        _heartbeat_error = "";
        _heartbeat_ok_ms = millis();
        return;
    }
    // The device was removed in the cloud, or its token no longer valid: link again
    if (code == 404 || code == 401 || code == 403) {
        AppLogger::log("WARN", "[CLOUD] The cloud no longer knows this stick (HTTP %d): unlinking", code);
        _unlink = true;
        return;
    }
    Guard g(_mutex);
    String e = code < 0 ? error : String("HTTP ") + code;
    if (e != _heartbeat_error) AppLogger::log("WARN", "[CLOUD] Heartbeat failed: %s", e.c_str());
    _heartbeat_error = e;
}

void CloudClient::applyCredentials(const StickClaim::Credentials& c) {
    if (!_zabbix) return;
    ZabbixClient::Managed m;
    m.server = c.zabbix_server;
    m.port = c.zabbix_port;
    m.host = c.zabbix_host;
    m.server_ca_pem = c.server_ca_pem;
    m.cert_pem = c.cert_pem;
    m.key_pem = StickIdentity::privateKeyPem();
    m.device_id = c.device_id;
    _zabbix->setManaged(m);
}

bool CloudClient::loadCredentials(StickClaim::Credentials& c) {
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, true)) return false;
    c.device_id = p.getString("did", "").c_str();
    c.name = p.getString("name", "").c_str();
    c.organization = p.getString("org", "").c_str();
    c.agent_token = p.getString("tok", "").c_str();
    c.zabbix_server = p.getString("zsrv", "").c_str();
    c.zabbix_port = p.getUShort("zport", 10051);
    c.zabbix_host = p.getString("zhost", "").c_str();
    c.ca_pem = p.getString("ca", "").c_str();
    c.server_ca_pem = p.getString("sca", "").c_str();
    c.cert_pem = p.getString("cert", "").c_str();
    p.end();
    return c.complete();
}

bool CloudClient::saveCredentials(const StickClaim::Credentials& c) {
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return false;
    bool ok = p.putString("did", c.device_id.c_str()) && p.putString("tok", c.agent_token.c_str()) &&
              p.putString("zsrv", c.zabbix_server.c_str()) && p.putUShort("zport", c.zabbix_port) &&
              p.putString("zhost", c.zabbix_host.c_str()) && p.putString("sca", c.server_ca_pem.c_str()) &&
              p.putString("cert", c.cert_pem.c_str());
    // Shown only; an empty name or CA is fine
    p.putString("name", c.name.c_str());
    p.putString("org", c.organization.c_str());
    p.putString("ca", c.ca_pem.c_str());
    p.end();
    return ok;
}

void CloudClient::eraseCredentials() {
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return;
    p.clear();
    p.end();
}
