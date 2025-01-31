#pragma once

#include "inttypes.h"
#include "stdbool.h"

#define CH446Q_DATA_PIN 20
#define CH446Q_SCLK_PIN 21
#define CH446Q_STROBE_PIN 22

// Set IO pins to output.
void CH446Q_init();
// Address: bits 6-0: AY2,AY1,AY0,AX3,AX2,AX1,AX0 
void CH446Q_set(uint8_t address, bool value);
// Set all switches to off state.
void CH446Q_reset();