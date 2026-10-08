#include "button.h"
#include "system/system.h"
#include "config/pins.h"
#include "run/run_service.h"
#include "connectivity/network.h"
#include "led/led.h"

// Polling the pin from loop() can miss a short press entirely if loop() is
// ever blocked elsewhere (e.g. streaming a CSV download over the web
// server, or an SD write) for longer than the press lasts. An interrupt
// catches both edges the instant the pin changes, independent of what
// loop() is doing.
//
// A press is only accepted once its full low->high cycle has been seen and
// measured to last at least MIN_PRESS_MS — that measurement happens
// entirely inside the ISR using edge timestamps, so it doesn't matter how
// late buttonUpdate() gets around to reading the result. This is what
// rejects switch bounce and brief electrical noise (e.g. from the WiFi
// radio switching off when a run starts) — a noise glitch doesn't hold the
// pin low for MIN_PRESS_MS, a finger does. Checking the pin's state at some
// later, uncertain instant (the previous approach) misses genuinely short
// presses whenever loop() is busy past the moment the finger lifts.
static constexpr uint32_t MIN_PRESS_MS = 30;
static constexpr uint32_t ACTION_LOCKOUT_MS = 200;

// Stopping a run needs a deliberately longer hold than starting one — a
// stray knee/leg bump against the button mid-ride is brief, but stopping
// on purpose means actually noticing and holding it. Starting stays a
// quick click since there's nothing to accidentally lose there.
static constexpr uint32_t STOP_HOLD_MS = 600;

// Holding the button this long (while not recording) resets connectivity
// settings to factory defaults and reboots into the fallback AP — an
// emergency escape hatch for a rider who forgets a custom AP/WiFi password
// and can no longer reach the settings page to fix it.
static constexpr uint32_t LONG_PRESS_RESET_MS = 10000;

static volatile bool pressActive = false;
static volatile uint32_t pressStartMs = 0;
static volatile bool clickPending = false;
static volatile bool longPressFired = false;

static void IRAM_ATTR handleButtonISR() {
    uint32_t now = millis();
    bool low = (digitalRead(PIN_BUTTON) == LOW);

    if (low) {
        if (!pressActive) {
            pressActive = true;
            pressStartMs = now;
            longPressFired = false;
        }
        // else: bounce while already down — keep the original start time
    } else if (pressActive) {
        pressActive = false;

        // Stopping requires holding past STOP_HOLD_MS; starting (or any
        // other state) only needs to clear the noise-rejection floor.
        uint32_t requiredMs = (getState() == RECORDING) ? STOP_HOLD_MS : MIN_PRESS_MS;

        // A long press already fired its own action while held (see
        // buttonUpdate) — don't also trigger a short-click start/stop here.
        if (!longPressFired && (now - pressStartMs >= requiredMs))
            clickPending = true;
        // else: released too quickly to be a real press — noise, ignore
    }
}

void buttonInit() {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), handleButtonISR, CHANGE);
}

void buttonUpdate() {
    // Long-press-to-reset: polled here (rather than in the ISR) since it
    // needs to fire while the button is still held down, not on release.
    // Only armed outside a recording — resetting connectivity mid-run would
    // restart the device and lose the run.
    if (pressActive && !longPressFired) {
        uint32_t heldMs = millis() - pressStartMs;

        if (heldMs >= LONG_PRESS_RESET_MS && getState() != RECORDING) {
            longPressFired = true;

            Serial.println("BUTTON LONG PRESS -> resetting connectivity");

            // Blink to confirm before the restart — ledUpdate() won't get
            // another chance to run since wifiResetCredentials() reboots.
            ledSetAlert(true);
            for (int i = 0; i < 6; i++) {
                ledUpdate();
                delay(120);
            }

            wifiResetCredentials();  // saves defaults and restarts the device
        }
    }

    static uint32_t lastAction = 0;

    if (!clickPending)
        return;

    clickPending = false;

    uint32_t now = millis();
    if (now - lastAction < ACTION_LOCKOUT_MS)
        return;

    lastAction = now;

    Serial.println("BUTTON PRESSED");

    if (getState() == READY)
    {
        RunService::start();
    }
    else if (getState() == RECORDING)
    {
        RunService::stop();
    }
}
