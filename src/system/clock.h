#pragma once

#include <Arduino.h>

// Wall-clock time, so a run can be stamped with when it was recorded.
//
// The ESP32 has no battery-backed RTC — after every power-up it has no idea
// what day it is until something tells it. Three things can, whichever shows
// up first; the clock then just keeps running until power-off:
//
//   GPS      date/time straight from the satellites. Usually valid well
//            before a position fix, but not indoors or under heavy cover.
//   NTP      when the device is on the home network with internet.
//   BROWSER  the phone/computer showing the web UI hands over its own time.
//            The only one available on the trail in AP mode with no sky.
//
// Listed most to least trusted: a source is never overridden by a less
// trusted one (a phone with a wrong clock can't undo a GPS time), but may be
// refreshed by an equal or better one.
//
// Nothing here is used for timing — run durations and sample timestamps stay
// on millis(), which setting the clock doesn't touch.
enum class ClockSource : uint8_t {
    NONE = 0,
    BROWSER = 1,
    NTP = 2,
    GPS = 3
};

// Call once the station interface is up. Returns immediately; the answer
// arrives in the background, or never if the network has no internet.
void clockStartNtp();

// UTC fields as decoded from an NMEA sentence.
void clockSetFromGps(int year, int month, int day, int hour, int minute, int second);

// Seconds since the Unix epoch, UTC. Returns false if it was rejected
// (implausible value, or a more trusted source has already set the clock).
bool clockSetFromBrowser(uint32_t epochSeconds);

bool clockIsSet();

// Seconds since the Unix epoch, UTC — 0 while the clock isn't set.
uint32_t clockNow();

ClockSource clockSource();
const char* clockSourceName();
