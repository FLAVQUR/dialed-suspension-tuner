#pragma once
// Shared by the files of the run service (run_*.cpp); not part of its
// interface, which is run_service.h.
#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>

// A run's files, all in the card's root.
inline String csvPath(const String& run) { return "/" + run + ".CSV"; }
inline String jsonPath(const String& run) { return "/" + run + ".JSON"; }
inline String csvbPath(const String& run) { return "/" + run + ".CSVB"; }

// run_compact.cpp: writes the run's .CSVB from its .CSV, replacing any.
bool writeCompactCsv(const String& runName);

// run_files.cpp
// The names of the files in the card's root, read from the directory alone.
std::vector<String> rootFileNames();
// One more than the highest run number on the card.
int nextRunNumber();
// The IMU's "Zero IMU" reference into a run's .JSON as imu_up, if it has one.
void addImuReference(JsonDocument& doc);
