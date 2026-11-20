#ifndef MDNS_ANNOUNCER_H
#define MDNS_ANNOUNCER_H

#include <Arduino.h>

/**
 * @brief Announces the board on mDNS once its link is up (builds with -DENABLE_MDNS):
 * <hostname>.local, the NUT server as _nut._tcp and the web UI as _http._tcp, with TXT
 * records vendor/model/fw on the NUT service so an app can name the board before it
 * connects. Without ENABLE_MDNS every call does nothing.
 *
 * Call loop() from the main loop; it is cheap when nothing changed. A changed hostname
 * restarts the responder, a changed model only updates the TXT records.
 *
 * With -DMDNS_SHARED_NAME="tescom-link" the board also answers as <shared>.local, an
 * easy address to type, but only when no other board on the network answers to it
 * already (asked once when the responder starts); the first one keeps it. The address
 * follows a new IP from DHCP.
 */
class MdnsAnnouncer {
public:
    void loop(bool connected, const String& hostname, const String& mfr, const String& model,
              uint16_t nut_port);

    // <shared>.local when this board answers to it, otherwise empty
    String sharedName() const;

private:
    bool _started = false;
    String _hostname;
    String _txt_model;
    String _txt_mfr;
    bool _shared_checked = false;
    uint32_t _shared_ip = 0;  // the address the shared name answers with, 0 when not ours

    void shareName();
};

#endif // MDNS_ANNOUNCER_H
