#include <Arduino.h>
#include <driver/adc.h>
#include "battery.h"
#include "config/pins.h"
#include "system/fast_adc.h"

#define SAMPLE_COUNT      50
#define SAMPLE_INTERVAL_MS 4     // 50 samples × 4ms = 200ms per burst
#define MEASURE_PERIOD_MS 3000   // start a new burst this often — voltage doesn't move fast

static float filteredVoltage = 0.0f;
static float burstVoltage = 0.0f;
static uint32_t lastSampleTime = 0;
static uint32_t lastBurstTime = 0;
static long sampleAccum = 0;
static int samplesDone = 0;
static bool sampling = false;

void batteryInit() {
    analogReadResolution(12);
    fastAdcAttach(PIN_BATTERY);

    // Chip-wide ESP32 erratum workaround (ECO 3.11, quoted in driver/adc.h):
    // every time the SAR ADC's power switch turns on, GPIO36 and GPIO39 are
    // pulled low for ~80ns. Without this the ADC is powered up for each read,
    // and the suspension pots alone do ~2000 reads/s — and GPIO39 carries the
    // IMU's INT line, which the BNO08x's SPI HAL reads as "a packet is ready"
    // whenever it is low. Holding the ADC powered costs ~1mA and removes the
    // glitches for every ADC user on the chip. Lives here because this is
    // where the ADC is set up; it is not specific to the battery.
    adc_power_acquire();

    sampling = true;  // take a first reading right away instead of waiting a full period
}

void batteryUpdate() {
    if (!sampling) {
        if (millis() - lastBurstTime < MEASURE_PERIOD_MS)
            return;

        sampling = true;
        sampleAccum = 0;
        samplesDone = 0;
    }

    if (millis() - lastSampleTime < SAMPLE_INTERVAL_MS)
        return;

    lastSampleTime = millis();

    sampleAccum += fastAdcReadMilliVolts(PIN_BATTERY);
    samplesDone++;

    if (samplesDone >= SAMPLE_COUNT) {
        float avgMv = sampleAccum / (float)SAMPLE_COUNT;

        // 1:2 voltage divider → ×2, mV → V
        float voltage = (avgMv / 1000.0f) * 2.0f;
        burstVoltage = voltage;

        if (filteredVoltage == 0.0f)
            filteredVoltage = voltage;
        else
            filteredVoltage = filteredVoltage + 0.1f * (voltage - filteredVoltage);

        sampling = false;
        lastBurstTime = millis();
    }
}

float readBatteryVoltage() {
    return filteredVoltage;
}

float batteryBurstVoltage() {
    return burstVoltage;
}


// The share of a riding day left before the low-battery cutoff (3.30V, see
// low_battery.cpp), read off the drain test of 2026-10-27/28: this cell, a
// timed run every 12 minutes, the smoothed voltage with WiFi on. So "50%" is
// half the time left, not half the charge. The old guess of a curve had 0% at
// the same voltage but read low below 3.7V (10% with 16% of the time left).
// The first 0.1V is the cell's surface charge, gone in the first 8 minutes.
static const float PERCENT_CURVE[][2] = {
    {4.20f, 100}, {4.10f, 97}, {4.05f, 90}, {4.00f, 81}, {3.95f, 71},
    {3.90f, 64},  {3.85f, 60}, {3.80f, 47}, {3.75f, 37}, {3.70f, 32},
    {3.60f, 25},  {3.50f, 16}, {3.40f, 7},  {3.30f, 0},
};

float batteryPercent(float v) {
    constexpr int n = sizeof(PERCENT_CURVE) / sizeof(PERCENT_CURVE[0]);

    if (v >= PERCENT_CURVE[0][0]) return 100.0f;
    if (v <= PERCENT_CURVE[n - 1][0]) return 0.0f;

    for (int i = 1; i < n; i++) {
        if (v >= PERCENT_CURVE[i][0]) {
            float v0 = PERCENT_CURVE[i][0], p0 = PERCENT_CURVE[i][1];
            float v1 = PERCENT_CURVE[i - 1][0], p1 = PERCENT_CURVE[i - 1][1];
            return p0 + (v - v0) * (p1 - p0) / (v1 - v0);
        }
    }
    return 0.0f;
}