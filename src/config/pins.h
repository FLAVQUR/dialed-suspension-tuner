#pragma once
#include <Arduino.h>

constexpr uint8_t PIN_SCK = 18;

constexpr uint8_t PIN_SD_CS = 5;
constexpr uint8_t PIN_SD_MOSI = 23;
constexpr uint8_t PIN_SD_MISO = 19;

constexpr uint8_t PIN_GPS_RX = 16;
constexpr uint8_t PIN_GPS_TX = 17;

constexpr uint8_t PIN_BATTERY = 34;

// BNO08x on its own SPI bus (HSPI) — kept off the SD card's VSPI bus so the
// two never contend for the same hardware peripheral across cores. See
// SCL/SCK/RX, SDA/MISO/TX and ADDR/MOSI on the breakout: those pins are
// shared with I2C and only carry these SPI functions once PS1/PS0 are wired
// to select SPI mode.
// Moved from GPIO14 (2026-09-15) — that pin's pad lost its ring during
// rework and stopped taking solder. GPIO27 sits right next to it in the
// same header row and was already free (LED moved off it to GPIO22), so
// the IMU bus stays fully on this one column with no other side effects.
constexpr uint8_t PIN_IMU_SCK  = 27;
constexpr uint8_t PIN_IMU_MISO = 35;
constexpr uint8_t PIN_IMU_MOSI = 13;
// Swapped with PIN_BUTTON (2026-09-13): 26 sits on the same header column as
// the rest of this bus, so the whole 6-wire IMU harness (SCK/MISO/MOSI/CS/
// INT/RST) can now run to a single connector instead of splitting one wire
// off to the other side of the board. The button has no bus to keep
// together, so it took GPIO4 (IMU's old CS pin) with no downside.
constexpr uint8_t PIN_IMU_CS   = 26;
// Moved from GPIO15 (2026-09-13): 15 sits on the opposite header column from
// the rest of this bus, forcing this wire alone to cross the whole board,
// and it's also a boot-strapping pin. 39 is input-only (fine, this is an
// ESP32-side input), physically adjacent to the other IMU pins, and not a
// strap — but has no internal pull, so it needs that external 10k pull-up
// to 3.3V during the rewire.
constexpr uint8_t PIN_IMU_INT  = 39;
constexpr uint8_t PIN_IMU_RST  = 25;

// Moved from GPIO27 to the right-column GPIO22 (2026-09-14) — single wire,
// no bus to keep together, so it can sit wherever is easiest to solder.
// GPIO22 is free, not a strapping pin, and not shared with flash or UART0.
constexpr uint8_t PIN_LED = 22;

constexpr uint8_t PIN_BUTTON = 4;

constexpr uint8_t PIN_SUSPENSION_FRONT = 32;
constexpr uint8_t PIN_SUSPENSION_REAR = 33;

// GPIO36 (SENSOR_VP, ADC1_CH0) is free. It briefly held a 3.3V rail monitor
// for the SD write-failure investigation (2026-09-26, since removed): it was
// the only unclaimed ADC1 pin, the other five being 32/33 suspension, 34
// battery, 35 IMU MISO and 39 IMU INT. Any future analog input has to be on
// ADC1 too — ADC2 (0/2/4/12-15/25-27) is unusable while Wi-Fi is running.