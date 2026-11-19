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
#include <esp_attr.h>
#include <esp_crt_bundle.h>
#include <esp_random.h>
#include <esp_http_client.h>
#include <string.h>
#include <time.h>

#ifndef CLOUD_URL
#define CLOUD_URL "https://connect.tescom.gr"
#endif
#ifndef CLOUD_TENANT
#define CLOUD_TENANT "tescom"
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

namespace {

const char* NVS_NAMESPACE = "cloud";
// Written at the factory (scripts/stick_qr.py): the secret the QR code on the stick and its box carries
const char* FACTORY_NAMESPACE = "factory";
const uint32_t HEARTBEAT_MS = 30000;
const int HTTP_TIMEOUT_MS = 15000;
// Answers are small JSON; the claim's credentials are the largest (two PEMs, a JWT)
const size_t MAX_BODY = 16 * 1024;

// The code shown on the setup page, across the restart that closes the setup network
// (survives esp_restart(), not a power cycle; the cloud's code lasts 15 min anyway)
#define SETUP_CODE_MAGIC 0x53434F44u // "SCOD"
struct SetupCode {
    uint32_t magic;
    char user_code[24];
    char verify_url[160];
    char poll_token[96];
    uint32_t expires_in_s;
    uint32_t interval_s;
};
RTC_NOINIT_ATTR SetupCode s_setup_code;

bool copyField(char* dst, size_t size, const std::string& v) {
    if (v.size() >= size) return false;
    memcpy(dst, v.c_str(), v.size() + 1);
    return true;
}

std::string fieldOf(const char* v, size_t size) {
    return std::string(v, strnlen(v, size));
}

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
    if (_mutex) return;
    _ups = ups;
    _net = net;
    _zabbix = zabbix;
    _mutex = xSemaphoreCreateMutex();
    StickClaim::Credentials c;
    if (loadCredentials(c)) {
        _flow.restore(c);
        applyCredentials(c);
        AppLogger::log("INFO", "[CLOUD] Linked to Tescom Connect as \"%s\" (%s)", c.name.c_str(), c.organization.c_str());
    } else if (s_setup_code.magic == SETUP_CODE_MAGIC) {
        StickClaim::PendingCode p;
        p.user_code = fieldOf(s_setup_code.user_code, sizeof(s_setup_code.user_code));
        p.verify_url = fieldOf(s_setup_code.verify_url, sizeof(s_setup_code.verify_url));
        p.poll_token = fieldOf(s_setup_code.poll_token, sizeof(s_setup_code.poll_token));
        p.expires_in_s = s_setup_code.expires_in_s;
        p.interval_s = s_setup_code.interval_s;
        if (_flow.resume(p, millis()))
            AppLogger::log("INFO", "[CLOUD] Code %s from the setup page: still waiting for its approval", p.user_code.c_str());
    } else {
        AppLogger::log("INFO", "[CLOUD] Not linked to Tescom Connect yet: a code is asked for once on Wi-Fi");
    }
    s_setup_code.magic = 0;  // used once
    xTaskCreatePinnedToCore(taskEntry, "cloud", 8192, this, 1, nullptr, 0);
}

String CloudClient::mac() const {
    uint8_t m[6];
    WiFi.macAddress(m);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return buf;
}

bool CloudClient::connected() const {
    return (_net && _net->isConnected()) || WiFi.status() == WL_CONNECTED;
}

bool CloudClient::hasAnswer() const {
    if (!_mutex) return false;
    Guard g(_mutex);
    return _flow.state() == StickClaim::State::CLAIMED ||
           (_flow.state() == StickClaim::State::PENDING && !_flow.preApproved());
}

void CloudClient::keepCodeForRestart() {
    s_setup_code.magic = 0;
    if (!_mutex) return;
    StickClaim::PendingCode p;
    {
        Guard g(_mutex);
        if (!_flow.pending(p, millis())) return;
    }
    if (!copyField(s_setup_code.user_code, sizeof(s_setup_code.user_code), p.user_code) ||
        !copyField(s_setup_code.verify_url, sizeof(s_setup_code.verify_url), p.verify_url) ||
        !copyField(s_setup_code.poll_token, sizeof(s_setup_code.poll_token), p.poll_token))
        return;
    s_setup_code.expires_in_s = p.expires_in_s;
    s_setup_code.interval_s = p.interval_s;
    s_setup_code.magic = SETUP_CODE_MAGIC;
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
            // What the owner still has to do in Tescom Connect ("ups", "plan"), and where
            doc["next"] = c.next.size() ? c.next : (c.zabbix().empty() ? "ups" : "sending");
            doc["manageUrl"] = c.manage_url.size() ? c.manage_url : (std::string(CLOUD_URL) + "/?stick=" + c.device_id);
            doc["sending"] = !c.zabbix().empty();
            doc["heartbeatOk"] = _heartbeat_sent && _heartbeat_error.length() == 0;
            if (_heartbeat_sent && _heartbeat_error.length() == 0) doc["lastHeartbeatAgoS"] = (now - _heartbeat_ok_ms) / 1000;
            if (_heartbeat_error.length()) doc["lastError"] = _heartbeat_error;
        } else if (_flow.lastError().size()) {
            doc["lastError"] = _flow.lastError();
        }
        doc["connected"] = connected();
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
        if (!connected()) continue;
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
        req.secret = factorySecret().c_str();
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
            if (_flow.preApproved())
                AppLogger::log("INFO", "[CLOUD] Already in a Tescom Connect account (its QR code): picking up the credentials");
            else
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
    StickClaim::ControlLogin control;
    if (_control_ups.length()) {
        String user, pass;
        controlLogin(user, pass);
        control.username = user.c_str();
        control.password = pass.c_str();
        control.ups = _control_ups.c_str();
        control.address = _net ? _net->localIP().c_str() : "";
    }
    int code = httpsPost(String(CLOUD_URL) + "/v1/agent/heartbeat",
                         StickClaim::heartbeatBody(FIRMWARE_VERSION, _started_at_ms, link, status.c_str(),
                                                   _control_ups.length() ? &control : nullptr),
                         token, out, error);
    _last_heartbeat = millis();
    _heartbeat_sent = true;
    if (code == 200) {
        StickClaim::HeartbeatAnswer answer;
        bool read = StickClaim::parseHeartbeat(out, answer);
        StickClaim::Credentials changed;
        bool zabbix_changed = false;
        {
            Guard g(_mutex);
            if (_heartbeat_error.length()) AppLogger::log("INFO", "[CLOUD] Heartbeat works again");
            _heartbeat_error = "";
            _heartbeat_ok_ms = millis();
            if (read) {
                StickClaim::Credentials c = _flow.credentials();
                zabbix_changed = answer.known && answer.zabbix != c.zabbix();
                if (answer.known) c.setZabbix(answer.zabbix);
                if (answer.next.size()) c.next = answer.next;
                _flow.restore(c);
                changed = c;
            }
        }
        // Readings start when the owner's UPS is added and on a plan, and stop when it no longer is
        if (zabbix_changed) {
            saveCredentials(changed);
            applyCredentials(changed);
            if (changed.zabbix().empty())
                AppLogger::log("INFO", "[CLOUD] Not sending readings: %s",
                               changed.next == "plan" ? "the UPS needs a cloud plan" : "the UPS is not added in Tescom Connect yet");
            else
                AppLogger::log("INFO", "[CLOUD] Sending readings to %s as %s", changed.zabbix_server.c_str(), changed.zabbix_host.c_str());
        }
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
    if (c.zabbix().empty()) {
        _zabbix->clearManaged();
        return;
    }
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

void CloudClient::controlLogin(String& username, String& password) {
    static const char* const NS = "stick_ctl";
    username = "tescom-connect";
    Preferences p;
    if (p.begin(NS, true)) {
        password = p.getString("pw", "");
        p.end();
    }
    if (password.length() >= 16) return;
    // 24 characters from the hardware random generator, letters and digits only (no NUT quoting)
    static const char ALPHABET[] = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    char buf[25];
    for (int i = 0; i < 24; i++) buf[i] = ALPHABET[esp_random() % (sizeof(ALPHABET) - 1)];
    buf[24] = 0;
    password = buf;
    if (p.begin(NS, false)) {
        p.putString("pw", password);
        p.end();
    }
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
    c.next = p.getString("next", "").c_str();
    c.manage_url = p.getString("murl", "").c_str();
    p.end();
    return c.complete();
}

bool CloudClient::saveCredentials(const StickClaim::Credentials& c) {
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return false;
    bool ok = p.putString("did", c.device_id.c_str()) && p.putString("tok", c.agent_token.c_str()) &&
              p.putString("sca", c.server_ca_pem.c_str()) && p.putString("cert", c.cert_pem.c_str());
    // Empty until the cloud says where readings go (putString of "" writes nothing, so remove)
    auto put = [&p](const char* key, const std::string& v) {
        if (v.empty())
            p.remove(key);
        else
            p.putString(key, v.c_str());
    };
    put("zsrv", c.zabbix_server);
    put("zhost", c.zabbix_host);
    p.putUShort("zport", c.zabbix_port);
    // Shown only
    put("name", c.name);
    put("org", c.organization);
    put("ca", c.ca_pem);
    put("next", c.next);
    put("murl", c.manage_url);
    p.end();
    return ok;
}

std::string CloudClient::factorySecret() {
    Preferences p;
    if (!p.begin(FACTORY_NAMESPACE, true)) return "";
    std::string s = p.getString("qr", "").c_str();
    p.end();
    return s;
}

void CloudClient::eraseCredentials() {
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return;
    p.clear();
    p.end();
}
