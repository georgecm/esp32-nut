#ifndef DRIVER_REGISTRY_H
#define DRIVER_REGISTRY_H

#include <stdint.h>

class IUPSDriver;
class ISerialPort;

/**
 * @brief Driver selection by USB VID/PID (review S3).
 *
 * One table instead of an if/else in USBHostUPS::claimInterface(), shared with the
 * fixture replay tests. The first matching row wins; GenericDriver otherwise. A VID/PID
 * that several upstream subdrivers share is told apart by the manufacturer and product
 * strings, in the order of usbhid-ups subdriver_list (US-097).
 */
namespace DriverRegistry {
    static const uint16_t ANY_PID = 0xFFFF;

    // New driver for the device, owned by the caller (never null). mfr and product are the
    // USB strings of the device, nullptr or "" when it has none.
    IUPSDriver* create(uint16_t vid, uint16_t pid, const char* mfr = "", const char* product = "");

    // A Q* (nutdrv_qx) bridge: a HID device without UPS usages that a Q* driver reads
    bool isQx(uint16_t vid, uint16_t pid);

    // A UPS behind a USB-serial bridge that USBHostUPS reads over its own serial host
    // instead of HID: the CH340 (1a86:7523) of the Tescom Prime
    bool isSerial(uint16_t vid, uint16_t pid);
    // New driver for a serial device (isSerial), reading through port; owned by the caller,
    // nullptr when the device is not one
    IUPSDriver* createSerial(uint16_t vid, uint16_t pid, ISerialPort* port);
}

#endif // DRIVER_REGISTRY_H
