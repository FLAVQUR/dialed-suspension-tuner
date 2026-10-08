#include <driver/adc.h>
#include <esp_adc_cal.h>
#include "fast_adc.h"

// The same reference the Arduino core characterises with when the chip has
// no better one burnt in (esp_adc_cal_characterize() prefers the eFuse
// values by itself).
static constexpr uint32_t DEFAULT_VREF_MV = 1100;

static esp_adc_cal_characteristics_t characteristics;
static bool characterized = false;

void fastAdcAttach(uint8_t pin) {
    // The core's own one-time setup for this pin: analog mode and 11dB, the
    // attenuation every analog input on this board uses.
    analogSetPinAttenuation(pin, ADC_11db);
    adcAttachPin(pin);

    if (!characterized) {
        esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, DEFAULT_VREF_MV, &characteristics);
        characterized = true;
    }
}

uint32_t fastAdcReadMilliVolts(uint8_t pin) {
    int raw = adc1_get_raw((adc1_channel_t)digitalPinToAnalogChannel(pin));

    return esp_adc_cal_raw_to_voltage((uint32_t)raw, &characteristics);
}
