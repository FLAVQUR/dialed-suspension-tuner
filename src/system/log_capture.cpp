#include <rom/ets_sys.h>
#include "log_capture.h"

namespace {

constexpr size_t RING_BYTES = 3072;

char ring[RING_BYTES];
volatile size_t head = 0;       // where the next character goes
volatile size_t stored = 0;     // how many of the ring's bytes hold text

// The core's log_printf() ends in the ROM's ets_printf(), which sends each
// character through whatever function is installed here. Logging can come
// from any task (the SD writer, for the messages this is here for) or an
// interrupt, so this does the least it can: keep the character, pass it on
// to the serial port as before.
void IRAM_ATTR capture(char c) {
    ring[head] = c;
    head = (head + 1) % RING_BYTES;
    if (stored < RING_BYTES) stored = stored + 1;

    ets_write_char_uart(c);
}

} // namespace

void logCaptureInit() {
    ets_install_putc1(capture);
}

String logCaptureRead() {
    String out;

    const size_t count = stored;
    const size_t start = (head + RING_BYTES - count) % RING_BYTES;

    out.reserve(count);

    for (size_t i = 0; i < count; i++)
        out += ring[(start + i) % RING_BYTES];

    return out;
}

void logCaptureClear() {
    head = 0;
    stored = 0;
}
