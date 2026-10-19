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


float batteryPercent(float v) {
    float percent;

    if      (v >= 4.20f) percent = 100.0f;
    else if (v >= 4.00f) percent = 85.0f + (v - 4.00f) * 75.0f;
    else if (v >= 3.85f) percent = 60.0f + (v - 3.85f) * 166.0f;
    else if (v >= 3.70f) percent = 30.0f + (v - 3.70f) * 200.0f;
    else if (v >= 3.50f) percent = 10.0f + (v - 3.50f) * 100.0f;
    else                 percent =          (v - 3.30f) * 50.0f;

    if (percent > 100.0f) percent = 100.0f;
    if (percent < 0.0f)   percent = 0.0f;

    return percent;
}