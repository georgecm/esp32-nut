#include "board/board.h"

// Tescom USB-to-Wi-Fi stick (docs/tescom-stick.md): for now an ESP32-S3 DevKitC-1 N16R8
// with the UPS on the native USB OTG port. The access point keeps the shared development
// password until the per-unit secrets of the factory partition land (plan step 3).
static const BoardConfig TESCOM_STICK = {
    "tescom_stick",
    "Tescom USB-to-Wi-Fi stick",
    true,   // has_wifi
    false,  // has_ethernet
    { EthType::NONE, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 48, 38 },
    { 1, { { "otg", "UPS USB port", 0 } } },
    { "Tescom-Stick-Setup", "12345678", true, 3, 0, NetworkMode::WIFI },
};

const BoardConfig& board() {
    return TESCOM_STICK;
}
