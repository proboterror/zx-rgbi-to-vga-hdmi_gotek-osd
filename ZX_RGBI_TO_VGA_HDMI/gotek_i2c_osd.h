#pragma once

struct display
{
    int rows, cols, on;
    uint8_t text[4][40];
};

extern struct display i2c_display;

void osd_process();
void setup_i2c_slave();