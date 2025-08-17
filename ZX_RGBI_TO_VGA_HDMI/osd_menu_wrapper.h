#ifndef OSD_MENU_WRAPPER_H
#define OSD_MENU_WRAPPER_H

#include "g_config.h"

#ifndef MENU_ROWS
#define MENU_ROWS 11
#endif

#ifndef MENU_COLS
#define MENU_COLS 40
#endif

#define MENU_TIMEOUT_MS 10000
#define DEBOUNCE_DELAY_MS 200
#define LONG_PRESS_MS 2000
#define ENTER_BUTTON 0x04
#define INVERSION_SUBMENU_ITEMS 9

#ifdef __cplusplus
extern "C" {
#endif

void set_scanlines_mode();
bool osd_menu_is_active(void);

typedef enum {
    MENU_MODE_MAIN,
    MENU_MODE_CAPTURE,
    MENU_MODE_OUTPUT,
    MENU_MODE_INVERSION
} menu_mode_t;

// Индексы пунктов меню
typedef enum {
    MAIN_ITEM_CAPTURE = 0,
    MAIN_ITEM_OUTPUT  = 1,
    MAIN_ITEM_EXIT    = 2,
    MAIN_ITEM_SAVE    = 3,
} main_menu_item_t;

typedef enum {
    CAPTURE_ITEM_SYNC        = 0,
    CAPTURE_ITEM_PIXCLK      = 1,
    CAPTURE_ITEM_DIV_OR_FREQ = 2, // Делитель (EXT) или Частота (SELF)
    CAPTURE_ITEM_DELAY       = 3,
    CAPTURE_ITEM_SHX         = 4,
    CAPTURE_ITEM_SHY         = 5,
    CAPTURE_ITEM_INVERSION   = 6,
    CAPTURE_ITEM_BACK        = 7,
} capture_menu_item_t;

typedef enum {
    OUTPUT_ITEM_VIDEO    = 0,
    OUTPUT_ITEM_SCAN     = 1,
    OUTPUT_ITEM_BUFFER   = 2,
    OUTPUT_ITEM_MODE     = 3, // авто/ручной
    OUTPUT_ITEM_BACK     = 4,
} output_menu_item_t;

typedef struct {
    bool active;
    menu_mode_t current_mode;
    bool editing;
    uint32_t show_time;
    uint32_t last_button_time;
    uint8_t current_item;
    uint8_t submenu_selection;
    uint8_t first_visible_row;
    uint8_t items_count;
    settings_t* settings;
    char menu_display[MENU_ROWS][MENU_COLS + 1];
    bool visible_items[MENU_ROWS];
} menu_state_t;

void osd_menu_init(settings_t* settings);
void osd_menu_show(void);
void osd_menu_hide(void);
bool osd_menu_is_active(void);
menu_mode_t osd_menu_get_mode(void);
void osd_menu_process(uint8_t buttons);
void osd_menu_process_pending_updates(void);
void save_settings(settings_t *settings);
// Устанавливается один раз при старте системы, чтобы меню не дергало пины
void osd_menu_set_vga_connected(bool connected);

uint8_t osd_menu_rows_count(void);
const uint8_t* osd_menu_get_row_ptr(uint8_t row_idx);
void osd_menu_refresh_render_buf(void);

#ifdef __cplusplus
}
#endif

#endif