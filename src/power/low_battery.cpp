#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <driver/rtc_io.h>
#include "low_battery.h"
#include "battery.h"
#include "config/pins.h"
#include "system/fast_adc.h"
#include "system/system.h"
#include "run/run_service.h"
#include "gps/gps.h"
#include "imu/imu.h"
#include "led/led.h"

// Long enough that a dip — the radio coming back after a run, a burst of web
// requests — never fires it: the smoothed voltage has to stay under for this
// whole time. In the drain test the device ran another 33 minutes past 3.30V.
static constexpr uint32_t CUTOFF_HOLD_MS = 120000;

static constexpr uint64_t CHECK_INTERVAL_US = 10ULL * 60 * 1000 * 1000;

// No cell reads this low: the charger board's protection cuts at ~2.5V. A
// reading under it is the divider with nothing behind it (powered from USB
// with the switch off, say), and must not put the device to sleep for good.
static constexpr float NO_BATTERY_V = 2.5f;

// Survives deep sleep: tells a wake from the cutoff's own sleep apart from a
// power-on or a reset.
RTC_DATA_ATTR static bool asleepForBattery = false;

static uint32_t lowSinceMs = 0;

static float quickBatteryVoltage() {
    analogReadResolution(12);
    fastAdcAttach(PIN_BATTERY);

    uint32_t sum = 0;
    for (int i = 0; i < 32; i++) {
        sum += fastAdcReadMilliVolts(PIN_BATTERY);
        delay(2);
    }
    return sum / 32.0f / 1000.0f * 2.0f;   // 1:2 divider, as in battery.cpp
}

static void sleepFor(uint64_t us) {
    asleepForBattery = true;

    // The button wakes it: GPIO4 is one of the RTC pins, pulled up in sleep.
    rtc_gpio_init((gpio_num_t)PIN_BUTTON);
    rtc_gpio_set_direction((gpio_num_t)PIN_BUTTON, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BUTTON);
    rtc_gpio_pullup_en((gpio_num_t)PIN_BUTTON);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON, 0);

    esp_sleep_enable_timer_wakeup(us);

    // Keeps the line to the GPS held high (gpsSleep() set it): a floating
    // line would wake the module.
    gpio_deep_sleep_hold_en();

    Serial.flush();
    esp_deep_sleep_start();
}

void lowBatteryBootCheck() {
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && asleepForBattery) {
        float v = quickBatteryVoltage();

        if (v >= NO_BATTERY_V && v < LOW_BATTERY_RESUME_V) {
            Serial.printf("Battery %.2fV, still under %.2fV: back to sleep\n", v, LOW_BATTERY_RESUME_V);
            sleepFor(CHECK_INTERVAL_US);
        }
        Serial.printf("Battery %.2fV: charged, starting up\n", v);
    }

    asleepForBattery = false;

    // Going ahead with the boot: the button pin back to the digital side for
    // buttonInit(), and the IMU let out of reset, or imuInit() can't drive it.
    rtc_gpio_deinit((gpio_num_t)PIN_BUTTON);
    rtc_gpio_hold_dis((gpio_num_t)PIN_IMU_RST);
    rtc_gpio_deinit((gpio_num_t)PIN_IMU_RST);
}

static void cutOff(float v) {
    Serial.printf("Battery %.2fV, under %.2fV for 2 min: cutting off\n", v, LOW_BATTERY_CUTOFF_V);

    if (getState() == RECORDING)
        RunService::stopForLowBattery();
    else
        RunService::cancelPendingStart();

    ledShowLowBatteryCutoff();

    gpsSleep();

    // Asleep or held in reset, the BNO08x stops working either way, and the
    // next boot resets it. The reset is the fallback: with the read loop
    // bounded, the sensor took the command 3 times out of 3 (bench,
    // 2026-10-28). What each draws was not measured.
    if (!imuSleep()) {
        rtc_gpio_init((gpio_num_t)PIN_IMU_RST);
        rtc_gpio_set_direction((gpio_num_t)PIN_IMU_RST, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level((gpio_num_t)PIN_IMU_RST, 0);
        rtc_gpio_hold_en((gpio_num_t)PIN_IMU_RST);
    }

    SD.end();
    WiFi.mode(WIFI_OFF);

    // A button still held down would wake it the moment it slept.
    uint32_t t0 = millis();
    while (digitalRead(PIN_BUTTON) == LOW && millis() - t0 < 5000) {
        esp_task_wdt_reset();
        delay(10);
    }

    sleepFor(CHECK_INTERVAL_US);
}

void lowBatteryUpdate() {
    float v = readBatteryVoltage();

    // 0 before the first reading; under NO_BATTERY_V there is no cell.
    if (v < NO_BATTERY_V || v >= LOW_BATTERY_CUTOFF_V) {
        lowSinceMs = 0;
        return;
    }

    uint32_t now = millis();
    if (lowSinceMs == 0) {
        lowSinceMs = now | 1;
        return;
    }

    if (now - lowSinceMs >= CUTOFF_HOLD_MS)
        cutOff(v);
}
