#pragma once

// What is on the device. FIRMWARE_VERSION is `git describe` at build time:
// the last tag, the commits since it, the commit, and "-dirty" if the build
// included uncommitted changes. FIRMWARE_BUILD is the date and time of the
// build — the way to confirm an OTA push actually landed when the code is
// still uncommitted and the version alone would not change.
//
// Neither needs any upkeep: tools/version_stamp.py writes them on every
// build. They are defined in version.cpp rather than here so that a new
// stamp recompiles one file, and so that the stamp is the build's and not
// that of whichever file happened to be recompiled.
extern const char FIRMWARE_VERSION[];
extern const char FIRMWARE_BUILD[];
