#pragma once

#include <Arduino.h>

// Keeps the tail of what the Arduino core and its libraries log (log_e /
// log_w — the SD driver's "crc error", "Wait Failed", "token error" and so
// on) in memory, to be read back over the API (GET /api/debug/log).
//
// Those messages say *why* a write to the card failed, which nothing above
// the driver can tell: FatFS reduces every reason to "disk error" and the
// Arduino File wrapper drops even that. They normally go to the serial port,
// where on this device nobody is listening — and only exist at all when the
// firmware is built with CORE_DEBUG_LEVEL at 2 or more (see platformio.ini).
void logCaptureInit();

// Everything captured since the last clear, oldest first (the last few KB of
// it, if there was more).
String logCaptureRead();
void logCaptureClear();
