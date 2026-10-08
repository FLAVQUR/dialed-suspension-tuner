#include <SD.h>
#include <SPI.h>
#include "storage.h"
#include "config/pins.h"

bool storageInit() {
    Serial.println("Initializing Storage...");

    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    SPI.begin(PIN_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

    delay(100);

    // As fast as this goes, and for reliability rather than speed. Writes to
    // the card fail now and then with a bit error on the link (the card's
    // CRC catches it, RunService recovers), and measured on 2026-10-07 the
    // rate of those follows how long the bus is busy, not how fast it is
    // clocked: 3.3 failures a minute at 4MHz, 1.2 at 10MHz, 0.4 at 20MHz —
    // about half an error per second of bus activity at all three. A slower
    // clock keeps every transfer on the wires for longer and is hit more.
    //
    // So do not lower this to "give the wiring more margin". That is what
    // was done in September (20MHz -> 10MHz, after RUN0019 lost its file and
    // RUN0020 stalled for 330-400ms), on the belief that speed was the
    // problem; those were this same failure, at a time when nothing detected
    // or recovered it, and the stalls are the card's own — flushes take the
    // same ~8ms, with the same occasional 200ms one, at either speed.
    constexpr uint32_t SD_SPI_HZ = 20000000;

    bool ok = false;

    for (int i = 0; i < 5; i++) {
        if (SD.begin(PIN_SD_CS, SPI, SD_SPI_HZ)) {
            ok = true;
            break;
        }

        delay(500);
    }

    if (!ok) {
        Serial.println("Storage failed!");
        return false;
    }
    else {
        Serial.println("Storage Ready!");
    }

    uint8_t cardType = SD.cardType();

    Serial.print("Card type: ");
    Serial.println(cardType);
    Serial.print("Card size: ");
    Serial.print(SD.cardSize() / (1024 * 1024));
    Serial.println(" MB");
    
    return true;
}

File storageGet(const char* filename) {
    return SD.open(filename);
}

File storageOpen(const char* filename) {
    return SD.open(filename, FILE_WRITE);
}

void storageClose(File& file) {
    if (file) {
        file.flush();
        file.close();
    }
}

bool storageExists(const char* filename) {
    return SD.exists(filename);
}

void storageFlush(File& file) {
    if (file) {
        file.flush();
    }
}

bool storageRemove(const char* filename) {
    return SD.remove(filename);
}
