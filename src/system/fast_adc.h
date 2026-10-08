#pragma once

#include <Arduino.h>

// analogReadMilliVolts() without the work it repeats on every call.
//
// The Arduino core's version re-attaches the pin (pin mode, attenuation) and
// rebuilds the ADC's calibration curve from eFuse each time it is called, and
// only then converts. Measured on this board that is ~350us per reading —
// and the suspension takes eight of them per sample, which made the ADC 79%
// of every pass of loop() and the one thing setting the sampling rate
// (profiled 2026-10-07: 2.8ms of a 3.5ms pass, 277 rows/s with 500
// configured).
//
// Here the pin is set up once and the curve is built once; a reading is then
// the conversion itself plus the curve lookup. The result is the same number
// analogReadMilliVolts() returns — same attenuation, width and reference —
// so calibration points captured with either are interchangeable.
//
// ADC1 pins only (GPIO32-39), which is all this board uses: ADC2 is not
// usable while Wi-Fi runs.

// Call once per pin, after analogReadResolution() if that is ever changed.
void fastAdcAttach(uint8_t pin);

uint32_t fastAdcReadMilliVolts(uint8_t pin);
