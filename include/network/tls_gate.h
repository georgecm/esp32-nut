#ifndef TLS_GATE_H
#define TLS_GATE_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * @brief One TLS session at a time: each costs about 40 KB of heap, and the cloud task and
 * the Zabbix sender would otherwise open theirs together.
 */
class TlsGate {
public:
    TlsGate() {
        if (!mutex()) mutex() = xSemaphoreCreateMutex();
        xSemaphoreTake(mutex(), portMAX_DELAY);
    }
    ~TlsGate() { xSemaphoreGive(mutex()); }
    // Called once from setup(), before any task can race on the lazy creation
    static void init() {
        if (!mutex()) mutex() = xSemaphoreCreateMutex();
    }
    TlsGate(const TlsGate&) = delete;
    TlsGate& operator=(const TlsGate&) = delete;

private:
    static SemaphoreHandle_t& mutex() {
        static SemaphoreHandle_t m = nullptr;
        return m;
    }
};

#endif // TLS_GATE_H
