#include <Arduino.h>
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include "gps.h"
#include "config/pins.h"
#include "system/system.h"
#include "system/clock.h"

static HardwareSerial GPSSerial(2);
static TinyGPSPlus gps;

static bool connected = false;
static bool hasFix = false;

static constexpr uint32_t CLOCK_RESYNC_INTERVAL_MS = 600000;
static constexpr uint32_t CLOCK_MAX_FIX_AGE_MS = 1500;
static bool clockSyncedFromGps = false;
static uint32_t lastClockSyncMs = 0;

// The module sends ~3.8KB/s at this baud rate, and the serial driver holds
// what loop() hasn't collected yet. Its default of 256 bytes is 66ms of
// that — less than the pause when the WiFi radio shuts down at the start of
// a run (~85ms). A buffer that overflowed came back as a stall of its own:
// see gpsUpdate().
static constexpr size_t GPS_RX_BUFFER_BYTES = 1024;

// What the logger asks for on every row (500 times a second), kept as plain
// floats and refreshed only when the module has actually said something new
// (a handful of times a second). TinyGPS++ works these out in double
// precision on every call — lat() alone is a double divide and add — and
// the ESP32 does doubles in software, so reading the same unchanged position
// 500 times a second cost more than everything else in a row put together.
static float cachedLat = 0.0f;
static float cachedLon = 0.0f;
static float cachedAlt = 0.0f;
static float cachedSpeedKmh = 0.0f;
static float cachedHdop = 0.0f;
static int cachedSats = 0;

bool gpsInit() {
    Serial.println("Initializing GPS...");

    GPSSerial.setRxBufferSize(GPS_RX_BUFFER_BYTES);     // has to come before begin()
    GPSSerial.begin(38400, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

    connected = false;
    hasFix = false;

    return true;
}

void gpsUpdate() {
    // Whatever has arrived, in one non-blocking read, and no more than this
    // per call — the rest waits for the next one, a couple of ms away.
    //
    // This used to be `while (available()) encode(read())`. After loop()
    // had been held up long enough to overflow the driver's buffer,
    // available() went on reporting bytes that read() could not deliver, and
    // that loop span on nothing for 120-135ms — measured on every run, right
    // after the pause that caused it, so an ~85ms hole in the data became a
    // ~200ms one.
    uint8_t buffer[128];
    size_t count = GPSSerial.read(buffer, sizeof(buffer));

    bool newSentence = false;

    for (size_t i = 0; i < count; i++) {
        if (gps.encode((char)buffer[i]))
            newSentence = true;
    }

    if (newSentence) {
        cachedLat = (float)gps.location.lat();
        cachedLon = (float)gps.location.lng();
        cachedAlt = (float)gps.altitude.meters();
        cachedSpeedKmh = (float)gps.speed.kmph();
        cachedHdop = (float)gps.hdop.hdop();
        cachedSats = (int)gps.satellites.value();

        if (gps.location.isValid())
            hasFix = true;
    }

    if (gps.charsProcessed() > 100) {
        connected = true;
    }

    // Hand the satellites' date/time to the system clock (see clock.h): as
    // soon as it's first valid — typically well before a position fix —
    // then again every so often to cancel the ESP32's own drift. The age
    // check keeps a time decoded seconds ago from being applied as "now".
    // Only looked at when a sentence has just come in: without a plausible
    // date (indoors, say) the sync never succeeds, and there is no point
    // asking again until the module has said something new.
    if (newSentence &&
        (!clockSyncedFromGps || millis() - lastClockSyncMs > CLOCK_RESYNC_INTERVAL_MS) &&
        gps.date.isValid() && gps.time.isValid() && gps.time.age() < CLOCK_MAX_FIX_AGE_MS)
    {
        clockSetFromGps(gps.date.year(), gps.date.month(), gps.date.day(),
                        gps.time.hour(), gps.time.minute(), gps.time.second());

        // Only counts once it was actually accepted — a module that hasn't
        // heard a satellite yet reports its default date, which is refused.
        if (clockSource() == ClockSource::GPS) {
            clockSyncedFromGps = true;
            lastClockSyncMs = millis();
        }
    }

    // The dashboard's GPS health badge (see /api/status) reads this
    // directly — previously it was fed from gpsInit()'s always-true
    // return value instead, so it never actually reflected whether the
    // module was there at all.
    components.gps = connected;
}

bool gpsHasFix() {
    return hasFix;
}

float gpsLatitude() {
    return cachedLat;
}

float gpsLongitude() {
    return cachedLon;
}

float gpsAltitude() {
    return cachedAlt;
}

float gpsSpeedKmh() {
    return cachedSpeedKmh;
}

int gpsSatellites() {
    return cachedSats;
}

float gpsHdop() {
    return cachedHdop;
}
