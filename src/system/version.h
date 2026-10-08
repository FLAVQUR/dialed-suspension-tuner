#pragma once

// Compile-time build stamp rather than a manually-bumped version number
// — with how often firmware gets pushed over OTA during development,
// remembering to bump a version string is exactly the kind of thing
// that's easy to forget right when it matters most (confirming an OTA
// push actually landed). __DATE__/__TIME__ update automatically on every
// build with zero upkeep.
constexpr const char* FIRMWARE_BUILD = __DATE__ " " __TIME__;
