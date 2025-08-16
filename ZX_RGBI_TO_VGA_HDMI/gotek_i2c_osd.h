#pragma once

struct display
{
    int rows, cols, on;
    uint8_t text[4][40];
};

extern struct display i2c_display;

void osd_process();
void setup_i2c_slave();

// Функции для работы с OSD строкой Gotek
const char* get_gotek_osd_line(uint8_t line_index);
bool is_gotek_osd_active(void);
void set_gotek_osd_visibility(bool visible);

#define OSD_BUTTON_LEFT 1
#define OSD_BUTTON_RIGHT 2
#define OSD_BUTTON_SELECT 4

void set_osd_buttons(uint8_t buttons);