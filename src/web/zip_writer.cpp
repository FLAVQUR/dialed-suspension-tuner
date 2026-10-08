#include "zip_writer.h"
#include "storage/storage.h"
#include <algorithm>
#include <esp_task_wdt.h>

namespace {

uint32_t crc32Table[256];
bool crc32TableReady = false;

void ensureCrc32Table() {
    if (crc32TableReady) return;

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32Table[i] = c;
    }

    crc32TableReady = true;
}

// Standard CRC32 (as used by ZIP/PNG/etc). Fed incrementally across reads
// so the whole file never has to be held in RAM. `state` starts at
// 0xFFFFFFFF and needs the same final XOR applied once feeding is done.
uint32_t crc32Feed(uint32_t state, const uint8_t* buf, size_t len) {
    ensureCrc32Table();
    for (size_t i = 0; i < len; i++)
        state = crc32Table[(state ^ buf[i]) & 0xFF] ^ (state >> 8);
    return state;
}

void appendU16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
}

void appendU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}

void appendStr(std::vector<uint8_t>& buf, const String& s) {
    for (size_t i = 0; i < s.length(); i++)
        buf.push_back((uint8_t)s[i]);
}

// A fixed, valid-but-meaningless DOS date/time (1980-01-01 00:00) — the
// ZIP format requires one, but nothing downstream cares about it here.
constexpr uint16_t DOS_DATE = 0x21;
constexpr uint16_t DOS_TIME = 0;

// See handleApiRunDownload() in server.cpp for why this is 16KB (not the
// original, much smaller size) and why the read buffers below are
// `static` rather than stack-local — same fix, same reasoning, applied
// here too since this handler shares the identical read-chunk/sendContent
// pattern and was just as exposed to Nagle's-algorithm-per-small-write
// overhead on a multi-MB run.
constexpr size_t CHUNK_SIZE = 16384;

// Shared by both passes below (CRC pass, then send pass) — they never run
// concurrently, and a separate 16KB array for each was part of what
// overflowed the ESP32's DRAM budget once server.cpp also grew its own
// 16KB buffers (see the shared downloadBuf there for the same fix).
static uint8_t chunkBuf[CHUNK_SIZE];

struct EntryInfo {
    String nameInZip;
    uint32_t size = 0;
    uint32_t crc = 0;
    uint32_t localHeaderOffset = 0;
};

} // namespace

bool sendZipResponse(WebServer& server, const std::vector<ZipEntry>& entries, const String& downloadName) {
    std::vector<EntryInfo> infos;
    infos.reserve(entries.size());

    // Pass 1: validate every file opens, and compute size + CRC32 up
    // front — the local file header needs both before the file's data
    // can be written, and deferring them with a trailing data descriptor
    // isn't worth the extra complexity for files this small.
    for (const auto& entry : entries) {
        File f = storageGet(entry.sourcePath.c_str());

        if (!f) {
            server.send(404, "text/plain", "File not found: " + entry.sourcePath);
            return false;
        }

        uint32_t crc = 0xFFFFFFFF;
        int n;

        // Reading a multi-MB run CSV twice (once here for CRC, again
        // below to actually send it) with no watchdog reset in between
        // is exactly the kind of long, uninterrupted block that can
        // outlast the 8s watchdog (see main.cpp) and reboot the device
        // mid-download.
        while ((n = f.read(chunkBuf, sizeof(chunkBuf))) > 0) {
            crc = crc32Feed(crc, chunkBuf, n);
            esp_task_wdt_reset();
        }

        EntryInfo info;
        info.nameInZip = entry.nameInZip;
        info.size = f.size();
        info.crc = crc ^ 0xFFFFFFFF;
        infos.push_back(info);

        f.close();
    }

    // Fixed layout (local headers + data, then the central directory,
    // then the end record) computed up front so Content-Length can be
    // set before any body bytes go out.
    uint32_t offset = 0;
    for (auto& info : infos) {
        info.localHeaderOffset = offset;
        offset += 30 + info.nameInZip.length() + info.size;
    }

    const uint32_t centralDirStart = offset;
    uint32_t centralDirSize = 0;
    for (const auto& info : infos)
        centralDirSize += 46 + info.nameInZip.length();

    const uint32_t totalSize = centralDirStart + centralDirSize + 22;

    server.sendHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
    server.setContentLength(totalSize);
    server.send(200, "application/zip", "");
    server.client().setNoDelay(true);

    // Pass 2: stream each entry's local file header followed by its raw
    // (uncompressed/"stored") data.
    for (const auto& entry : entries) {
        const auto it = std::find_if(infos.begin(), infos.end(), [&](const EntryInfo& i) { return i.nameInZip == entry.nameInZip; });
        const EntryInfo& info = *it;

        std::vector<uint8_t> header;
        appendU32(header, 0x04034b50);
        appendU16(header, 20);           // version needed to extract
        appendU16(header, 0);            // general purpose bit flag
        appendU16(header, 0);            // compression method: stored
        appendU16(header, DOS_TIME);
        appendU16(header, DOS_DATE);
        appendU32(header, info.crc);
        appendU32(header, info.size);    // compressed size == uncompressed (stored)
        appendU32(header, info.size);
        appendU16(header, info.nameInZip.length());
        appendU16(header, 0);            // extra field length
        appendStr(header, info.nameInZip);

        server.sendContent((const char*)header.data(), header.size());

        File f = storageGet(entry.sourcePath.c_str());
        int n;

        while ((n = f.read(chunkBuf, sizeof(chunkBuf))) > 0) {
            server.sendContent((const char*)chunkBuf, n);
            esp_task_wdt_reset();
        }

        f.close();
    }

    // Central directory: one record per file, pointing back at its local
    // header offset.
    for (const auto& info : infos) {
        std::vector<uint8_t> record;
        appendU32(record, 0x02014b50);
        appendU16(record, 20);           // version made by
        appendU16(record, 20);           // version needed to extract
        appendU16(record, 0);            // general purpose bit flag
        appendU16(record, 0);            // compression method: stored
        appendU16(record, DOS_TIME);
        appendU16(record, DOS_DATE);
        appendU32(record, info.crc);
        appendU32(record, info.size);
        appendU32(record, info.size);
        appendU16(record, info.nameInZip.length());
        appendU16(record, 0);            // extra field length
        appendU16(record, 0);            // file comment length
        appendU16(record, 0);            // disk number start
        appendU16(record, 0);            // internal file attributes
        appendU32(record, 0);            // external file attributes
        appendU32(record, info.localHeaderOffset);
        appendStr(record, info.nameInZip);

        server.sendContent((const char*)record.data(), record.size());
    }

    // End of central directory record.
    std::vector<uint8_t> eocd;
    appendU32(eocd, 0x06054b50);
    appendU16(eocd, 0);                       // disk number
    appendU16(eocd, 0);                       // disk where CD starts
    appendU16(eocd, (uint16_t)infos.size());  // CD records on this disk
    appendU16(eocd, (uint16_t)infos.size());  // total CD records
    appendU32(eocd, centralDirSize);
    appendU32(eocd, centralDirStart);
    appendU16(eocd, 0);                       // comment length

    server.sendContent((const char*)eocd.data(), eocd.size());

    return true;
}
