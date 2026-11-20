#ifndef WEB_CONFIG_SERVER_H
#define WEB_CONFIG_SERVER_H

#include <functional>
#include <WebServer.h>
#include <DNSServer.h>
#include "core/config_manager.h"
#include "IUpsSource.h"
#include "USBHostUPS.h"
#include "AuthPolicy.h"
#include "AuthService.h"
#include "CredentialStore.h"
#include "LoginThrottle.h"
#include "network/zabbix_client.h"
#include "network/cloud_client.h"
#include "SessionTable.h"
#include "SetupHold.h"
#include "RecoveryWindow.h"
#include "DiagnosticLED.h"
#include "core/board_settings_store.h"
#include "core/auto_shutdown.h"
#include "core/hostname_store.h"
#include "network/auth_platform.h"
#include "network/network_manager.h"

class WebConfigServer {
public:
    WebConfigServer(ConfigManager& config_mgr);
    void begin(bool isAPMode);
    void loop();
    void setUPS(USBHostUPS* ups);
    // The Link status page shows the easy mDNS name when this board has it
    void setMdns(const class MdnsAnnouncer* mdns) { mdns_announcer = mdns; }
    // Active network link: captive portal address, status text, MAC (US-090)
    void setNetwork(AppNetworkManager* network);
    // Double reset on a board without Wi-Fi: new web credentials without the current
    // password for 10 minutes (US-091, PDR 0002 D5)
    void openRecoveryWindow(uint32_t now);
    // Board page (US-094): settings loaded at boot, and the LED to apply changes at once
    void setBoardSettings(BoardSettingsStore* store, const BoardSettings& settings);
    void setStatusLed(DiagnosticLED* led);
    // Automatic shutdown on low battery (US-114): turned on and off from the Board page
    void setAutoShutdown(AutoShutdown* auto_shutdown);
    // Hostname of the Network page (US-113): store and name loaded at boot ("" = default)
    void setHostnameStore(HostnameStore* store, const String& saved);
    void setZabbixClient(ZabbixClient* client) { zabbix_client = client; }
    void setCloudClient(CloudClient* client) { cloud_client = client; }
    // Setup network only (docs/tescom-stick.md, "Setup"): saving the Wi-Fi keeps the access
    // point up next to the station for a while and starts this client, so the setup page
    // shows the Tescom Connect code before the board restarts on the Wi-Fi
    void setSetupCloud(CloudClient* client) { setup_cloud = client; }

private:
    WebServer server;
    DNSServer dnsServer;
    ConfigManager& config_mgr;
    AppNetworkManager* network = nullptr;
    bool is_ap_mode;

    // Autenticazione della Web UI (ADR 0009); l'ordine di dichiarazione è quello di costruzione
    EspAuthCrypto auth_crypto;
    NvsAuthStorage auth_storage;
    CredentialStore credentials;
    LoginThrottle throttle;
    // failures() del throttle all'ultimo WARN 429: un solo log per finestra di attesa
    uint32_t throttle_logged_failures = 0;
    SessionTable sessions;
    AuthService auth;
    RecoveryWindow recovery;
    BoardSettingsStore* board_store = nullptr;
    BoardSettings board_settings = {};
    DiagnosticLED* status_led = nullptr;
    AutoShutdown* auto_shutdown = nullptr;
    HostnameStore* hostname_store = nullptr;
    ZabbixClient* zabbix_client = nullptr;
    CloudClient* cloud_client = nullptr;
    CloudClient* setup_cloud = nullptr;
    SetupHold setup_hold;
    String saved_hostname;

    void handleConnect();
    // The Wi-Fi networks around, for the setup page (an async scan: {"scanning":true} first)
    void handleWifiScan();
    // The full settings page
    void sendIndex();
    // GET /api/claim: the Tescom Connect claim (the app reads the code on the network)
    void sendClaim();
#ifdef LINK_WEB
    // The Tescom Connect Link's only routes: setup on the setup network, a status page on
    // the Wi-Fi, the claim; no login and no settings (the Connect app does the rest)
    void registerLinkRoutes();
    void handleLinkStatus();
#endif
    // AP+STA after the Wi-Fi was saved on the setup network, then the restart (SetupHold)
    void startSetupHold(const String& ssid, const String& password);
    void handleLogs();
    void handleNutConfig();
    void handleGetBoard();
    void handleSetBoard();
    // US-114: UPS connected and offering shutdown.default
    AutoShutdownAvailability autoShutdownAvailability() const;
    void handleSetHostname();
    void handleGetZabbix();
    void handleSetZabbix();
    void handleGetConfig();
    void handleUpsVars();
    void handleSystemStatus();
    void handleUpsCommands();
    void handleUpsCommand();
    void handleUpsVariables();
    void handleUpsVariable();
    void handleUpsVariableReset();
    // Chi ha inviato la richiesta, per il log: in AP non c'è sessione, quindi "(AP mode)"
    String webUiUser();
    // WARN per una scrittura o un reset rifiutati prima di arrivare all'host (with_value: scrittura)
    void logVariableRejected(const String& body, bool with_value, const String& response);

    void handleLoginPage();
    void handleAuthLogin();
    void handleAuthStatus();
    void handleAuthCredentials();
    void handleAuthLogout();
    void handleAuthLogoutAll();

    void handleOTAPage();
    void handleOTAUpload();

    // Guard: ogni rotta registrata passa da qui (nessun server.on() diretto)
    WebServer::THandlerFunction guarded(WebServer::THandlerFunction handler);
    // Valore del cookie di sessione della richiesta corrente
    String sessionToken();
    // Decisione per la richiesta corrente (uri e metodo), senza inviare risposte
    AuthDecision evaluateRequest();
    // false se ha già risposto (302 verso /login o 401 JSON)
    bool authorize();
    // Stessa decisione di authorize() senza risposta (callback di upload OTA)
    bool authorizedQuiet();
    // 415 se il Content-Type non è application/json (anti-CSRF); false se ha risposto
    bool requireJson();
    void sendAuth(const AuthResponse& res);

    bool should_restart = false;
    unsigned long restart_request_time = 0;
    // Upload OTA rifiutato a UPLOAD_FILE_START: chunk e fine sono ignorati (il guard risponde 401)
    bool ota_rejected = false;
    // Un upload multipart autorizzato è partito: senza, il handler finale non dichiara successo
    bool ota_upload_started = false;
    // millis() a UPLOAD_FILE_START: durata dell'upload nel log di successo (US-085)
    uint32_t ota_start_ms = 0;
    // Dati, stato e comandi dell'UPS; usb_ups resta solo per l'export della diagnostica USB
    IUpsSource* ups_source = nullptr;
    USBHostUPS* usb_ups = nullptr;
    const class MdnsAnnouncer* mdns_announcer = nullptr;
};

#endif // WEB_CONFIG_SERVER_H
