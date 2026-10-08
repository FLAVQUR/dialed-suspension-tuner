#pragma once

#include <Arduino.h>

void ledInit();
void ledUpdate();

// Overrides the normal state-based color with a fast magenta blink —
// confirms the button long-press-to-reset-connectivity gesture.
void ledSetAlert(bool active);

// Overrides the normal state-based color with a fast red blink — the SD
// writer has stopped making progress mid-recording (see
// RunService::isWriteStalled()). Distinct from ledSetAlert() so the two
// can never be confused for each other.
void ledSetWriteStall(bool active);