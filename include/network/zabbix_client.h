#ifndef ZABBIX_CLIENT_H
#define ZABBIX_CLIENT_H

#include <Arduino.h>
#include "ZabbixSender.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class USBHostUPS;
class AppNetworkManager;

/**
 * @brief Sends the UPS readings to Zabbix as tescom-agent does on a Raspberry Pi
 * (docs/tescom-stick.md, "Zabbix"): the "Tescom UPS by NUT" template's trapper items,
 * every interval, over the sender protocol (plain TCP, port 10051 by default).
 *
 * Runs in its own task, so a slow or unreachable server never stalls the NUT server or
 * the web UI. While Zabbix cannot be reached the values wait in a backlog (oldest
 * dropped when full) and go out together when it answers again. Readings carry the
 * time from SNTP once synced; before that the server stamps them on arrival.
 *
 * Settings live in NVS namespace "zabbix"; a build can preset the server and port
 * (ZABBIX_DEFAULT_SERVER, ZABBIX_DEFAULT_PORT).
 */
class ZabbixClient {
public:
    void begin(USBHostUPS* ups, AppNetworkManager* net);

    Zabbix::Settings settings() const;
    // Validated by the caller (Zabbix::validate); saved to NVS and used from the next cycle
    bool save(const Zabbix::Settings& s);
    // Sends at once instead of at the next interval (the web UI's "Send now")
    void sendNow();
    // Settings and state as JSON for GET /api/zabbix
    String statusJson() const;

private:
    static void taskEntry(void* arg);
    void run();
    void collect(const Zabbix::Settings& s, uint64_t clock);
    bool sendBacklog(const Zabbix::Settings& s);
    bool sendBatch(const Zabbix::Settings& s, size_t count, Zabbix::Outcome& outcome, String& error);

    USBHostUPS* _ups = nullptr;
    AppNetworkManager* _net = nullptr;
    mutable SemaphoreHandle_t _mutex = nullptr;
    TaskHandle_t _task = nullptr;
    Zabbix::Settings _settings;
    Zabbix::Backlog _backlog;
    volatile bool _send_now = false;
    bool _sntp_started = false;

    // State for the web UI (under _mutex)
    bool _failing = false;
    uint32_t _sends_ok = 0;
    uint32_t _sends_failed = 0;
    uint32_t _last_ok_ms = 0;
    bool _ever_ok = false;
    String _last_error;
    uint32_t _last_processed = 0;
    uint32_t _last_failed = 0;
};

#endif // ZABBIX_CLIENT_H
