#ifndef SETUP_BUTTON_H
#define SETUP_BUTTON_H

#include <stdint.h>

/**
 * @brief The setup button held down (docs/tescom-stick.md, "Setup"): held for hold_ms while
 * the board runs, it opens the setup network, as the double reset does. Nothing is erased.
 *
 * The press only counts after the button was seen up once since boot: held while the board
 * starts, the same button (GPIO 0, BOOT) selects the ROM's download mode instead, and a
 * button stuck down must not restart the board in a loop. A release shorter than
 * debounce_ms is contact bounce, not a release.
 *
 * Pure logic, so it runs in the native unit tests. Times are millis(), wrap-safe.
 */
class SetupButton {
public:
    static const uint32_t DEFAULT_HOLD_MS = 10000;
    static const uint32_t DEFAULT_DEBOUNCE_MS = 50;
    // Held this long, the LED shows it (before the hotspot opens)
    static const uint32_t SHOW_MS = 1000;

    explicit SetupButton(uint32_t hold_ms = DEFAULT_HOLD_MS, uint32_t debounce_ms = DEFAULT_DEBOUNCE_MS)
        : _hold_ms(hold_ms), _debounce_ms(debounce_ms) {}

    // True once per press, when it has been held hold_ms
    bool update(bool pressed, uint32_t now) {
        if (!_started || pressed != _raw) {
            _started = true;
            _raw = pressed;
            _changed = now;
        }
        if (now - _changed < _debounce_ms) return false;
        if (!_raw) {
            // Up and settled: from now on a press counts
            _armed = true;
            _down = false;
            _fired = false;
            return false;
        }
        if (!_down) {
            _down = true;
            _pressed_at = _changed;
        }
        if (_armed && !_fired && now - _pressed_at >= _hold_ms) {
            _fired = true;
            return true;
        }
        return false;
    }

    // Held long enough to show on the LED, and not fired yet
    bool holding(uint32_t now) const { return _armed && _down && !_fired && now - _pressed_at >= SHOW_MS; }

private:
    uint32_t _hold_ms;
    uint32_t _debounce_ms;
    bool _started = false;
    bool _raw = false;
    bool _armed = false;
    bool _down = false;
    bool _fired = false;
    uint32_t _changed = 0;
    uint32_t _pressed_at = 0;
};

#endif // SETUP_BUTTON_H
