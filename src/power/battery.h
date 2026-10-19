#pragma once

#include <Arduino.h>

void batteryInit();
void batteryUpdate();
float readBatteryVoltage();

// The last 200ms burst on its own, before the smoothing readBatteryVoltage()
// applies (about 30s to settle): what a run's lowest voltage is taken from.
// 0 before the first burst.
float batteryBurstVoltage();
float batteryPercent(float voltage);