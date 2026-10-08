#pragma once

#include <Arduino.h>

void batteryInit();
void batteryUpdate();
float readBatteryVoltage();
float batteryPercent(float voltage);