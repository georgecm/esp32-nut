#ifndef CLOUD_CLIENT_H
#define CLOUD_CLIENT_H

#include <Arduino.h>
#include "StickClaim.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class USBHostUPS;
class AppNetworkManager;
class ZabbixClient;

/**
 * @brief The stick and Tescom Connect (docs/tescom-stick.md, "Setup"): once on Wi-Fi it
 * asks the cloud for a code without credentials, shows it (GET /api/claim, the web UI),
 * and polls until the owner approves it on bridge.connect.tescom.gr; then it keeps the
 * credentials in NVS ("cloud"), hands the Zabbix settings and certificate to the Zabbix
 * sender, and sends a heartbeat with its agent token.
 *
 * Built with -DENABLE_CLOUD_CLAIM (the tescom-stick env); the cloud's address is
 * CLOUD_URL (default https://bridge.connect.tescom.gr). HTTPS is checked against the
 * ESP-IDF certificate bundle.
 */
class CloudClient {
public:
    void begin(USBHostUPS* ups, AppNetworkManager* net, ZabbixClient* zabbix);
    // The claim as JSON for GET /api/claim
    String statusJson() const;
    // Forget the claim and ask for a new code (the owner's "Unlink" in the web UI)
    void unlink();

private:
    static void taskEntry(void* arg);
    void run();
    void claimStep(uint32_t now);
    void heartbeat();
    void applyCredentials(const StickClaim::Credentials& c);
    bool loadCredentials(StickClaim::Credentials& c);
    bool saveCredentials(const StickClaim::Credentials& c);
    void eraseCredentials();
    String mac() const;

    USBHostUPS* _ups = nullptr;
    AppNetworkManager* _net = nullptr;
    ZabbixClient* _zabbix = nullptr;
    mutable SemaphoreHandle_t _mutex = nullptr;
    StickClaim::Flow _flow;
    volatile bool _unlink = false;
    uint32_t _last_heartbeat = 0;
    bool _heartbeat_sent = false;
    String _heartbeat_error;
    uint32_t _heartbeat_ok_ms = 0;
    uint64_t _started_at_ms = 0;
};

#endif // CLOUD_CLIENT_H
