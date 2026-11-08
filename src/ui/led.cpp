#include <FastLED.h>
#include "led.h"
#include "system/system.h"
#include "config/pins.h"
#include "power/battery.h"

// Below this, the LED starts weaving a periodic orange flash into whatever
// it's already showing — matches the "bad" battery tier already used in the
// web UI badges.
constexpr float LOW_BATTERY_PCT = 20.0f;

#define NUM_LEDS 1

static CRGB leds[NUM_LEDS];

static uint32_t lastBlink = 0;
static bool on = false;
static bool alertActive = false;
static bool writeStallActive = false;

void ledInit() {
    FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(50);

    leds[0] = CRGB::Yellow;
    FastLED.show();
}

// ledUpdate() runs on every pass of loop() and nearly always asks for the
// colour the LED already has. Sending it anyway cost ~0.3ms a pass (FastLED
// clocks the bits out and waits for them), which was 9% of the sampling
// loop spent repeating itself to one pixel. So the LED is only written when
// the colour changes — plus once a second regardless, so that a pixel
// knocked into the wrong state by a glitch on its data line doesn't stay
// that way for as long as the device sits in one steady colour.
constexpr uint32_t LED_REFRESH_MS = 1000;

static void setColor(CRGB color) {
    static bool shown = false;
    static uint32_t lastShownMs = 0;

    uint32_t now = millis();

    if (shown && color == leds[0] && now - lastShownMs < LED_REFRESH_MS)
        return;

    leds[0] = color;
    FastLED.show();

    shown = true;
    lastShownMs = now;
}

void ledShowLowBatteryCutoff() {
    for (int i = 0; i < 3; i++) {
        leds[0] = CRGB::Orange;
        FastLED.show();
        delay(250);
        leds[0] = CRGB::Black;
        FastLED.show();
        delay(250);
    }
}

void ledSetAlert(bool active) {
    alertActive = active;
}

void ledSetWriteStall(bool active) {
    writeStallActive = active;
}

void ledUpdate() {
    SystemState state = getState();

    uint32_t now = millis();

    // heartbeat timing — faster while an override (reset-confirmation
    // alert, or a stalled SD write) is active
    uint32_t blinkIntervalMs = (alertActive || writeStallActive) ? 100 : 400;
    bool beat = (now - lastBlink > blinkIntervalMs);
    if (beat) {
        lastBlink = now;
        on = !on;
    }

    if (alertActive) {
        setColor(on ? CRGB::Magenta : CRGB::Black);
        return;
    }

    // Takes priority over the normal RECORDING blue — this needs to grab
    // the rider's attention mid-ride, not blend in as a slightly
    // different shade of "everything's fine".
    if (writeStallActive) {
        setColor(on ? CRGB::Red : CRGB::Black);
        return;
    }

    // Weave a periodic orange flash into the normal state color rather
    // than replacing it outright — battery status stays visible without
    // hiding what the device is actually doing (a single LED has no room
    // for a second, independent indicator).
    bool batteryLow = batteryPercent(readBatteryVoltage()) < LOW_BATTERY_PCT;
    CRGB offColor = batteryLow ? CRGB::Orange : CRGB::Black;

    switch (state) {
        case BOOTING:
            setColor(on ? CRGB::Yellow : offColor);
            break;

        case READY:
            setColor(batteryLow ? (on ? CRGB::Green : offColor) : CRGB::Green);
            break;

        case RECORDING:
            setColor(on ? CRGB::Blue : offColor);
            break;

        case ERROR:
            setColor(batteryLow ? (on ? CRGB::Red : offColor) : CRGB::Red);
            break;
    }
}