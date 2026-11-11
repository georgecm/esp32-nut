#ifndef STICK_IDENTITY_H
#define STICK_IDENTITY_H

#include <Arduino.h>
#include <string>

/**
 * @brief The stick's own key pair (ECDSA P-256), made on the stick the first time and kept
 * in NVS ("stick"/"key"); the private key never leaves it. The cloud signs a certificate
 * for its public key when the stick is claimed, and Zabbix accepts that certificate only.
 */
namespace StickIdentity {

// The private key (PEM), made and saved when there is none; empty if that failed
std::string privateKeyPem();
// A certificate request for that key, subject CN=stick-<mac>; empty if it failed
std::string csrPem(const String& mac);
// Forget the key (a factory reset): the next claim makes a new one
void erase();

} // namespace StickIdentity

#endif // STICK_IDENTITY_H
