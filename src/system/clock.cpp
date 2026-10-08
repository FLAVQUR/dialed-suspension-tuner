#include <sys/time.h>
#include <time.h>
#include <esp_sntp.h>
#include "clock.h"

// Anything earlier than this is a clock that was never set (the ESP32 boots
// at 1970) or a GPS module still reporting its factory default date, not a
// real "now". 2024-01-01 00:00:00 UTC.
static constexpr uint32_t MIN_PLAUSIBLE_EPOCH = 1704067200UL;

// Written from loop() (GPS, browser) and from the SNTP task's callback.
// A single byte, and the worst a race could do is record one source where
// the other won by a few microseconds — both would have set a correct time.
static volatile ClockSource source = ClockSource::NONE;

static void applyEpoch(uint32_t epochSeconds, ClockSource from) {
    timeval tv = {};
    tv.tv_sec = (time_t)epochSeconds;
    settimeofday(&tv, nullptr);

    if (source != from) {
        source = from;
        Serial.printf("Clock set from %s: %lu\n", clockSourceName(), (unsigned long)epochSeconds);
    }
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). mktime() would do, but it reads the TZ environment and
// these fields are UTC by definition.
static int32_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;

    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + (int32_t)doe - 719468;
}

// SNTP sets the system time itself; this only records that it happened.
// Runs again on every periodic re-sync, which is fine — NTP and GPS agree
// to well under a second, so it never moves a GPS-set clock meaningfully.
static void onNtpSync(timeval*) {
    if (source < ClockSource::NTP) {
        source = ClockSource::NTP;
        Serial.println("Clock set from ntp");
    }
}

void clockStartNtp() {
    sntp_set_time_sync_notification_cb(onNtpSync);

    // UTC, no DST: the run's timestamp is stored as plain epoch seconds and
    // turned into local time by whichever browser displays it.
    configTime(0, 0, "pool.ntp.org", "time.google.com");
}

void clockSetFromGps(int year, int month, int day, int hour, int minute, int second) {
    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31)
        return;

    const uint32_t epoch = (uint32_t)daysFromCivil(year, month, day) * 86400UL
        + (uint32_t)hour * 3600UL + (uint32_t)minute * 60UL + (uint32_t)second;

    if (epoch < MIN_PLAUSIBLE_EPOCH)
        return;

    applyEpoch(epoch, ClockSource::GPS);
}

bool clockSetFromBrowser(uint32_t epochSeconds) {
    if (epochSeconds < MIN_PLAUSIBLE_EPOCH)
        return false;

    if (source > ClockSource::BROWSER)
        return false;

    applyEpoch(epochSeconds, ClockSource::BROWSER);
    return true;
}

bool clockIsSet() {
    return source != ClockSource::NONE && (uint32_t)time(nullptr) >= MIN_PLAUSIBLE_EPOCH;
}

uint32_t clockNow() {
    return clockIsSet() ? (uint32_t)time(nullptr) : 0;
}

ClockSource clockSource() {
    return source;
}

const char* clockSourceName() {
    switch (source) {
        case ClockSource::GPS:     return "gps";
        case ClockSource::NTP:     return "ntp";
        case ClockSource::BROWSER: return "browser";
        default:                   return "none";
    }
}
