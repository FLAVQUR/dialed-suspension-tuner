#pragma once
// Shared by the web server's files: the server object and every API handler,
// for serverInit() (server.cpp) to route. Not used outside src/web/.
#include "dialed_web_server.h"

extern DialedWebServer server;

// api_device.cpp
void handleApiStatus();
void handleApiTimeSet();
void handleApiWifiScan();
void handleApiWifiMode();
void handleApiWifiStatus();
void handleApiRunToggle();
void handleApiRunTimed();
void handleApiDebugLoop();
void handleApiDebugLog();
void handleApiSystem();

// api_config.cpp
void handleApiConfigGet();
void handleApiConfigPost();

// api_runs.cpp
void handleApiRuns();
void handleApiRunDownload();
void handleApiRunTelemetry();
void handleApiRunDownloadZip();
void handleApiRunDelete();
void handleApiRunNotes();
void handleApiRunBottomOuts();
void handleApiRunSetup();
void handleApiRunsClear();

// api_bike.cpp
void handleApiSuspensionCaptureZero();
void handleApiSuspensionCaptureMax();
void handleApiSuspensionSetTravel();
void handleApiSuspensionCaptureSag();
void handleApiSuspensionReset();
void handleApiBikesList();
void handleApiBikeActivate();
void handleApiBikeSave();
void handleApiBikeProfile();
void handleApiBikeDelete();
void handleApiBikeRename();
void handleApiSuspensionCalibrationGet();
void handleApiImuGet();
void handleApiImuZero();
void handleApiImuClear();
