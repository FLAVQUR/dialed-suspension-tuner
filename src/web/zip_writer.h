#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <vector>

struct ZipEntry {
    String sourcePath;  // filesystem path to read from (SD card)
    String nameInZip;   // filename shown inside the archive
};

// Streams a ZIP archive containing the given files as the HTTP response
// body — entries are stored uncompressed (no DEFLATE library needed, just
// correct framing), which is plenty for CSV/JSON files this size and
// keeps this self-contained. Sends its own error response and returns
// false if any source file can't be opened; on success, `server`'s
// response has already been fully sent.
bool sendZipResponse(WebServer& server, const std::vector<ZipEntry>& entries, const String& downloadName);
