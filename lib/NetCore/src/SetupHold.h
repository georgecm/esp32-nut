#ifndef SETUP_HOLD_H
#define SETUP_HOLD_H

#include <stdint.h>

/**
 * @brief How long the setup network stays up after the Wi-Fi was saved on it
 * (docs/tescom-stick.md, "Setup"): the stick joins the Wi-Fi with the access point still
 * on (AP+STA), so the setup page can show the Tescom Connect code before the phone
 * leaves. It ends, and the board restarts on the Wi-Fi, at the first of:
 * - max_ms after the Wi-Fi was saved (no internet, wrong password: as before, only later);
 * - grace_ms after the page first read an answer (the code, or "already in an account").
 *
 * Pure logic, so it runs in the native unit tests. Times are millis(), wrap-safe.
 */
class SetupHold {
public:
    static const uint32_t DEFAULT_MAX_MS = 60000;
    static const uint32_t DEFAULT_GRACE_MS = 8000;

    explicit SetupHold(uint32_t max_ms = DEFAULT_MAX_MS, uint32_t grace_ms = DEFAULT_GRACE_MS)
        : _max_ms(max_ms), _grace_ms(grace_ms) {}

    // The Wi-Fi was saved on the setup page (again: the hold starts over, e.g. a mistyped password)
    void start(uint32_t now) {
        _active = true;
        _started = now;
        _seen = false;
    }
    bool active() const { return _active; }
    uint32_t maxMs() const { return _max_ms; }

    // The setup page read the code (or the claimed state); only the first time counts
    void answerSeen(uint32_t now) {
        if (!_active || _seen) return;
        _seen = true;
        _seen_at = now;
    }

    // True once, when the setup network should close (the caller restarts)
    bool update(uint32_t now) {
        if (!_active) return false;
        bool over = (now - _started) >= _max_ms || (_seen && (now - _seen_at) >= _grace_ms);
        if (over) _active = false;
        return over;
    }

    // Seconds left at most, for the page
    uint32_t leftS(uint32_t now) const {
        if (!_active) return 0;
        uint32_t end = _max_ms;
        if (_seen && (_seen_at - _started) + _grace_ms < end) end = (_seen_at - _started) + _grace_ms;
        uint32_t gone = now - _started;
        return gone >= end ? 0 : (end - gone + 999) / 1000;
    }

private:
    uint32_t _max_ms;
    uint32_t _grace_ms;
    bool _active = false;
    uint32_t _started = 0;
    bool _seen = false;
    uint32_t _seen_at = 0;
};

#endif // SETUP_HOLD_H
