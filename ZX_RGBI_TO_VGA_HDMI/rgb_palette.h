#pragma once

#include "inttypes.h"

enum rgb_palette_index_t
{
    RGB_PALETTE_MIN,
    RGB_PALETTE_ALONE = RGB_PALETTE_MIN,
    RGB_PALETTE_PULSAR,
    RGB_PALETTE_ORTHODOX,
    RGB_PALETTE_EGA,
    RGB_PALETTE_MAX = RGB_PALETTE_EGA,
    RGB_PALETTE_SIZE
};

static const char* rgb_palette_names[RGB_PALETTE_SIZE] =
{
    "Alone",
    "Pulsar",
    "Orthodox",
    "EGA"
};

typedef uint8_t rgb_palette_t[16][3];

void set_rgb_palette_index(uint8_t palette_index);
uint8_t get_rgb_palette_index();

const rgb_palette_t* get_rgb_palette();