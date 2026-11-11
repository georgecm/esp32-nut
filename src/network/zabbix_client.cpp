#include "network/zabbix_client.h"
#include "USBHostUPS.h"
#include "core/app_logger.h"
#include "network/network_manager.h"
#include "network/tls_gate.h"
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include <memory>

#ifndef ZABBIX_DEFAULT_SERVER
#define ZABBIX_DEFAULT_SERVER ""
#endif
#ifndef ZABBIX_DEFAULT_PORT
#define ZABBIX_DEFAULT_PORT 10051
#endif

namespace {

const char* NVS_NAMESPACE = "zabbix";
// Values in one request: about 100 bytes of JSON each
const size_t BATCH_ITEMS = 200;
const uint32_t CONNECT_TIMEOUT_MS = 5000;
const uint32_t REPLY_TIMEOUT_MS = 5000;
// After 2023-11-14: the clock is set (SNTP), not the 1970 of a fresh boot
const time_t VALID_TIME = 1700000000;

class Guard {
public:
    explicit Guard(SemaphoreHandle_t m) : _m(m) { xSemaphoreTake(_m, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(_m); }

private:
    SemaphoreHandle_t _m;
};

uint64_t nowUnix() {
    time_t t = time(nullptr);
    return t > VALID_TIME ? (uint64_t)t : 0;
}

} // namespace

void ZabbixClient::begin(USBHostUPS* ups, AppNetworkManager* net) {
    _ups = ups;
    _net = net;
    _mutex = xSemaphoreCreateMutex();
    // About 100 bytes of heap per waiting value: the PSRAM boards keep more
    _backlog = Zabbix::Backlog(psramFound() ? 1500 : 300);

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    _settings.enabled = prefs.getBool("enabled", false);
    _settings.server = prefs.getString("server", ZABBIX_DEFAULT_SERVER).c_str();
    _settings.port = prefs.getUShort("port", ZABBIX_DEFAULT_PORT);
    _settings.host = prefs.getString("host", "").c_str();
    _settings.interval_s = prefs.getUShort("interval", 10);
    prefs.end();
    if (Zabbix::validate(_settings)) {
        AppLogger::log("WARN", "[ZABBIX] Saved settings not valid: disabled");
        _settings.enabled = false;
    }

    xTaskCreatePinnedToCore(taskEntry, "zabbix", 6144, this, 1, &_task, 0);
    AppLogger::log("INFO", "[ZABBIX] %s", _settings.enabled ? "Enabled" : "Disabled");
}

Zabbix::Settings ZabbixClient::settings() const {
    Guard g(_mutex);
    return _settings;
}

bool ZabbixClient::save(const Zabbix::Settings& s) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) return false;
    bool ok = prefs.putBool("enabled", s.enabled) && prefs.putString("server", s.server.c_str()) == s.server.size() &&
              prefs.putUShort("port", s.port) && prefs.putString("host", s.host.c_str()) == s.host.size() &&
              prefs.putUShort("interval", s.interval_s);
    prefs.end();
    if (!ok) return false;
    {
        Guard g(_mutex);
        bool target_changed = s.server != _settings.server || s.port != _settings.port || s.host != _settings.host;
        _settings = s;
        // Values for another server or host would land in the wrong place
        if (target_changed) _backlog.clear();
        _failing = false;
        _last_error = "";
    }
    AppLogger::log("INFO", "[ZABBIX] Settings saved: %s, %s:%u host \"%s\" every %u s", s.enabled ? "on" : "off",
                   s.server.c_str(), (unsigned)s.port, s.host.c_str(), (unsigned)s.interval_s);
    _send_now = true;
    return true;
}

void ZabbixClient::setManaged(const Managed& m) {
    {
        Guard g(_mutex);
        bool target_changed = !_managed || m.server != _managed_settings.server || m.host != _managed_settings.host;
        _managed = true;
        _managed_settings = m;
        if (target_changed) _backlog.clear();
        _failing = false;
        _last_error = "";
    }
    AppLogger::log("INFO", "[ZABBIX] Managed by Tescom Connect: %s:%u host \"%s\" over TLS", m.server.c_str(),
                   (unsigned)m.port, m.host.c_str());
    _send_now = true;
}

void ZabbixClient::clearManaged() {
    Guard g(_mutex);
    if (!_managed) return;
    _managed = false;
    _managed_settings = Managed();
    _backlog.clear();
    AppLogger::log("INFO", "[ZABBIX] No longer managed by Tescom Connect: back to the saved settings");
}

bool ZabbixClient::managed() const {
    Guard g(_mutex);
    return _managed;
}

Zabbix::Settings ZabbixClient::active() const {
    Guard g(_mutex);
    if (!_managed) return _settings;
    Zabbix::Settings s;
    s.enabled = true;
    s.server = _managed_settings.server;
    s.port = _managed_settings.port;
    s.host = _managed_settings.host;
    s.interval_s = _settings.interval_s;
    return s;
}

void ZabbixClient::sendNow() {
    _send_now = true;
}

String ZabbixClient::statusJson() const {
    JsonDocument doc;
    {
        Guard g(_mutex);
        bool on = _managed || _settings.enabled;
        doc["enabled"] = on;
        doc["server"] = _managed ? _managed_settings.server.c_str() : _settings.server.c_str();
        doc["port"] = _managed ? _managed_settings.port : _settings.port;
        doc["host"] = _managed ? _managed_settings.host.c_str() : _settings.host.c_str();
        doc["interval"] = _settings.interval_s;
        doc["managed"] = _managed;
        doc["tls"] = _managed;
        if (_managed) doc["deviceId"] = _managed_settings.device_id.c_str();
        JsonObject st = doc["status"].to<JsonObject>();
        st["state"] = !on ? "off" : _failing ? "failing" : _ever_ok ? "ok" : "waiting";
        if (_ever_ok) st["lastOkAgoS"] = (millis() - _last_ok_ms) / 1000;
        st["lastError"] = _last_error;
        st["lastProcessed"] = _last_processed;
        st["lastFailed"] = _last_failed;
        st["backlog"] = (uint32_t)_backlog.size();
        st["dropped"] = (uint32_t)_backlog.dropped();
        st["sendsOk"] = _sends_ok;
        st["sendsFailed"] = _sends_failed;
        st["timeSynced"] = nowUnix() != 0;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

void ZabbixClient::taskEntry(void* arg) {
    static_cast<ZabbixClient*>(arg)->run();
}

void ZabbixClient::run() {
    uint32_t last_cycle = 0;
    bool first = true;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        Zabbix::Settings s = active();
        if (!s.enabled) continue;

        bool connected = _net && _net->isConnected();
        if (connected && !_sntp_started) {
            configTime(0, 0, "pool.ntp.org", "time.google.com");
            _sntp_started = true;
        }

        uint32_t now = millis();
        bool due = first || (now - last_cycle) >= (uint32_t)s.interval_s * 1000;
        if (!due && !_send_now) continue;
        if (due) {
            first = false;
            last_cycle = now;
            collect(s, nowUnix());
        }
        _send_now = false;
        if (connected) sendBacklog(s);
    }
}

void ZabbixClient::collect(const Zabbix::Settings& s, uint64_t clock) {
    Zabbix::Link link = Zabbix::Link::OK;
    if (!_ups->isConnected()) link = Zabbix::Link::DRIVER_DOWN;
    else if (_ups->isDataStale()) link = Zabbix::Link::STALE;

    std::vector<Zabbix::Item> items;
    items.reserve(18);
    String status = _ups->getUPSStatusString();
    {
        auto data = _ups->getUPSData();
        Zabbix::items(s.host.c_str(), link, status.c_str(), &data.get(), clock, items);
    }
    Guard g(_mutex);
    _backlog.add(items);
}

bool ZabbixClient::sendBacklog(const Zabbix::Settings& s) {
    for (;;) {
        size_t count;
        {
            Guard g(_mutex);
            count = _backlog.size() < BATCH_ITEMS ? _backlog.size() : BATCH_ITEMS;
        }
        if (count == 0) return true;

        Zabbix::Outcome outcome;
        String error;
        bool ok = sendBatch(s, count, outcome, error);

        Guard g(_mutex);
        if (!ok) {
            _sends_failed++;
            _last_error = error;
            if (!_failing) {
                AppLogger::log("WARN", "[ZABBIX] Cannot send to %s:%u: %s; keeping values for later",
                               s.server.c_str(), (unsigned)s.port, error.c_str());
                _failing = true;
            }
            return false;
        }
        // The batch was taken from the front; values added meanwhile went to the back
        _backlog.popFront(count);
        if (_failing) {
            AppLogger::log("INFO", "[ZABBIX] Reachable again; sent %u waiting values", (unsigned)outcome.total);
            _failing = false;
        }
        if (outcome.failed > 0 && outcome.failed != _last_failed) {
            AppLogger::log("WARN", "[ZABBIX] Took %u of %u values; %u failed (host or template missing?)",
                           (unsigned)outcome.processed, (unsigned)outcome.total, (unsigned)outcome.failed);
        }
        _sends_ok++;
        _ever_ok = true;
        _last_ok_ms = millis();
        _last_error = "";
        _last_processed = outcome.processed;
        _last_failed = outcome.failed;
    }
}

bool ZabbixClient::sendBatch(const Zabbix::Settings& s, size_t count, Zabbix::Outcome& outcome, String& error) {
    std::string payload;
    {
        Guard g(_mutex);
        std::vector<Zabbix::Item> batch(_backlog.items().begin(), _backlog.items().begin() + count);
        payload = Zabbix::frame(Zabbix::requestJson(batch, nowUnix()));
    }

    // TLS with the stick's certificate when Tescom Connect manages the settings
    Managed tls;
    bool use_tls;
    {
        Guard g(_mutex);
        use_tls = _managed;
        if (use_tls) tls = _managed_settings;
    }
    std::unique_ptr<TlsGate> gate;
    // ESPLwIPClient: the base whose connect(host, port, timeout) and Client calls are virtual
    std::unique_ptr<ESPLwIPClient> owned;
    if (use_tls) {
        gate.reset(new TlsGate());
        auto* secure = new WiFiClientSecure();
        secure->setCACert(tls.server_ca_pem.c_str());
        secure->setCertificate(tls.cert_pem.c_str());
        secure->setPrivateKey(tls.key_pem.c_str());
        secure->setHandshakeTimeout(CONNECT_TIMEOUT_MS / 1000 + 5);
        owned.reset(secure);
    } else {
        owned.reset(new WiFiClient());
    }
    ESPLwIPClient& client = *owned;
    if (!client.connect(s.server.c_str(), s.port, CONNECT_TIMEOUT_MS)) {
        error = use_tls ? "could not connect (TCP or TLS handshake)" : "could not connect";
        return false;
    }
    size_t written = client.write((const uint8_t*)payload.data(), payload.size());
    if (written != payload.size()) {
        error = "connection closed while sending";
        client.stop();
        return false;
    }

    // Header first (13 bytes), then as many as it announces
    std::vector<uint8_t> reply;
    reply.reserve(256);
    size_t want = 13;
    uint32_t start = millis();
    while (reply.size() < want && (millis() - start) < REPLY_TIMEOUT_MS) {
        int avail = client.available();
        if (avail <= 0) {
            if (!client.connected()) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint8_t buf[256];
        int n = client.read(buf, avail < (int)sizeof(buf) ? avail : sizeof(buf));
        if (n > 0) reply.insert(reply.end(), buf, buf + n);
        if (want == 13 && reply.size() >= 13) {
            want = Zabbix::replyLength(reply.data(), reply.size());
            if (!want) break;
        }
    }
    client.stop();

    if (reply.empty()) {
        error = "timed out waiting for Zabbix";
        return false;
    }
    if (!Zabbix::parseReply(reply.data(), reply.size(), outcome)) {
        error = String("not a Zabbix reply (") + outcome.info.c_str() + ")";
        return false;
    }
    if (!outcome.ok) {
        error = String("Zabbix refused the batch: ") + outcome.info.c_str();
        return false;
    }
    return true;
}
