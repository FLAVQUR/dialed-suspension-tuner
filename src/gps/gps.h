#pragma once

#include <Arduino.h>

bool gpsInit();
void gpsUpdate();

// Real connection status lives in components.gps (see system.h) instead
// of a separate getter here — set inside gpsUpdate() as soon as the
// module starts sending valid NMEA data.
bool gpsHasFix();

// The last values the module reported, held as floats (see gps.cpp for why
// they are cached). Latitude and longitude are 0 until the first fix.
float gpsLatitude();
float gpsLongitude();
float gpsAltitude();
float gpsSpeedKmh();

int gpsSatellites();

float gpsHdop();