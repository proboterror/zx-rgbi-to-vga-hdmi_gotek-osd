#include "osd_menu_wrapper.h"
#include "gotek_i2c_osd.h"
#include "hardware/pio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "hardware/gpio.h"
#include "rgb_capture.h"
#include "vga.h"
#include "dvi.h" 
#include "v_buf.h"

typedef struct {
    volatile bool vga_connected_cached;
    menu_state_t menu;
    uint8_t render_rows_count;
    uint8_t render_buf[MENU_ROWS][MENU_COLS];
    volatile bool pending_ext_clk_update;
    volatile bool pending_video_restart;
    volatile bool pending_capture_restart;
    volatile bool pending_buffering_apply;
} osd_ctx_t;

static osd_ctx_t g_osd_ctx = {
    .vga_connected_cached = false,
    .menu = {0},
    .render_rows_count = 0,
    .render_buf = {{0}},
    .pending_ext_clk_update = false,
    .pending_video_restart = false,
    .pending_capture_restart = false,
    .pending_buffering_apply = false,
};

#define SELECTION_MARKER ">"
#define UNSELECTED_SPACE " "
#define EDIT_MARKER "*" 
#define MAX_DISPLAY_ROWS 4
#define MENU_VISIBLE_COLS 25

// Удобный алиас для поля состояния меню
#define MENU_STATE (g_osd_ctx.menu)

void osd_menu_set_vga_connected(bool connected) {
    g_osd_ctx.vga_connected_cached = connected;
}

uint8_t osd_menu_rows_count(void) {
    return g_osd_ctx.render_rows_count;
}

const uint8_t* osd_menu_get_row_ptr(uint8_t row_idx) {
    if (row_idx >= g_osd_ctx.render_rows_count) return NULL;
    return g_osd_ctx.render_buf[row_idx];
}

void osd_menu_refresh_render_buf(void) {
    // Очистка
    for (int r = 0; r < MENU_ROWS; r++) {
        memset(g_osd_ctx.render_buf[r], ' ', MENU_COLS);
    }
    g_osd_ctx.render_rows_count = 0;

    if (!MENU_STATE.settings) return;

    // Общая рамка вокруг
    const int content_width = MENU_VISIBLE_COLS - 2;

    // Верхняя граница:
    if (g_osd_ctx.render_rows_count < MENU_ROWS) {
        uint8_t *row = g_osd_ctx.render_buf[g_osd_ctx.render_rows_count++];
        row[0] = 0xDA; // ┌
        for (int c = 1; c < MENU_VISIBLE_COLS - 1; c++) row[c] = 0xC4; // ─
        row[MENU_VISIBLE_COLS - 1] = 0xBF; // ┐
        // Центрируем заголовок в верхней границе
        const char *title = " RGB2VGA Menu ";
        int title_len = (int)strlen(title);
        int max_len = MENU_VISIBLE_COLS - 2;
        if (title_len > max_len) title_len = max_len;
        int start_col = 1 + (max_len - title_len) / 2;
        memcpy(row + start_col, title, (size_t)title_len);
    }

    // Контентные строки
    int items = (MENU_STATE.current_mode == MENU_MODE_INVERSION) ? INVERSION_SUBMENU_ITEMS : MENU_STATE.items_count;
    for (int i = 0; i < items && g_osd_ctx.render_rows_count < MENU_ROWS; i++) {
        uint8_t *row = g_osd_ctx.render_buf[g_osd_ctx.render_rows_count++];
        // Левый бордер
        row[0] = 0xB3; // │
        // Заполняем контентную область пробелами
        for (int c = 0; c < content_width; c++) row[1 + c] = ' ';

    if (MENU_STATE.current_mode == MENU_MODE_INVERSION) {
            // В инверсии menu_display уже содержит маркер "> " / "  "
            size_t tlen = strnlen(MENU_STATE.menu_display[i], content_width);
            memcpy(row + 1, MENU_STATE.menu_display[i], tlen);
        } else {
            // Для остальных экранов добавляем маркер выбора
            const char sel = (i == MENU_STATE.current_item) ? (MENU_STATE.editing ? '*' : '>') : ' ';
            row[1] = (uint8_t)sel;
            size_t tlen = strnlen(MENU_STATE.menu_display[i], content_width - 1);
            memcpy(row + 2, MENU_STATE.menu_display[i], tlen);
        }
        // Правый бордер
        row[MENU_VISIBLE_COLS - 1] = 0xB3; // │
    }

    // Нижняя граница:
    if (g_osd_ctx.render_rows_count < MENU_ROWS) {
        uint8_t *row = g_osd_ctx.render_buf[g_osd_ctx.render_rows_count++];
        row[0] = 0xC0; // └
        for (int c = 1; c < MENU_VISIBLE_COLS - 1; c++) row[c] = 0xC4; // ─
        row[MENU_VISIBLE_COLS - 1] = 0xD9; // ┘
    }
}

// Маппинг русских символов
static const struct {
    const char *utf8;
    char font_code;
} char_map[] = {
    {"А", 0x80}, {"Б", 0x81}, {"В", 0x82}, {"Г", 0x83}, {"Д", 0x84},
    {"Е", 0x85}, {"Ж", 0x86}, {"З", 0x87}, {"И", 0x88}, {"Й", 0x89},
    {"К", 0x8A}, {"Л", 0x8B}, {"М", 0x8C}, {"Н", 0x8D}, {"О", 0x8E},
    {"П", 0x8F}, {"Р", 0x90}, {"С", 0x91}, {"Т", 0x92}, {"У", 0x93},
    {"Ф", 0x94}, {"Х", 0x95}, {"Ц", 0x96}, {"Ч", 0x97}, {"Ш", 0x98},
    {"Щ", 0x99}, {"Ъ", 0x9A}, {"Ы", 0x9B}, {"Ь", 0x9C}, {"Э", 0x9D},
    {"Ю", 0x9E}, {"Я", 0x9F},
    {"а", 0xA0}, {"б", 0xA1}, {"в", 0xA2}, {"г", 0xA3}, {"д", 0xA4},
    {"е", 0xA5}, {"ж", 0xA6}, {"з", 0xA7}, {"и", 0xA8}, {"й", 0xA9},
    {"к", 0xAA}, {"л", 0xAB}, {"м", 0xAC}, {"н", 0xAD}, {"о", 0xAE},
    {"п", 0xAF}, {"р", 0xE0}, {"с", 0xE1}, {"т", 0xE2}, {"у", 0xE3},
    {"ф", 0xE4}, {"х", 0xE5}, {"ц", 0xE6}, {"ч", 0xE7}, {"ш", 0xE8},
    {"щ", 0xE9}, {"ъ", 0xEA}, {"ы", 0xEB}, {"ь", 0xEC}, {"э", 0xED},
    {"ю", 0xEE}, {"я", 0xEF},

};


// Названия битов инверсии
static const char* inversion_bit_names[INVERSION_SUBMENU_ITEMS] = {
    "OSD          ",
    "CLC (частота)",
    "VS (КСИ)     ",
    "HS (ССИ)     ",
    "I (яркость)  ",
    "R (красный)  ",
    "G (зеленый)  ",
    "B (синий)    ",
    "Назад"
};

// Названия режимов видео
static const char* video_mode_names[] = {
    "DVI",
    "VGA 640x480",
    "VGA 800x600",
    "VGA 1024x768",
    "VGA 1280x1024 (div3)",
    "VGA 1280x1024 (div4)"
};

// Названия режимов синхронизации
static const char* sync_mode_names[] = {
    "Синхросмесь",
    "Раздельная"
};

// Названия режимов пиксельклока
static const char* pixel_clock_names[] = {
    "самос-я",
    "внешняя"
};

// Тексты пунктов для разных экранов меню
static const char* main_menu_items[] = {
    "Настройка захвата",
    "Настройка вывода",
    "Выйти",
    "Сохранить и выйти"
};

static const char* capture_menu_items[] = {
    "Синхр-ия: %s",
    "Пиксельклок: %s",
    "Делитель: %d",
    "Задержка: %d",
    "Частота: %d кГц",
    "Смещение X: %d",
    "Смещение Y: %d",
    "Инверсия",
    "Назад"
};

static const char* output_menu_items[] = {
    "Видео: %s",
    "Режим scanlines: %s",
    "Буферизация: %s",
    "Режим видео: %s",
    "Назад"
};

extern void save_settings(settings_t *settings);

static void utf8_to_font_str(const char *src, char *dst, size_t max_len) {
    size_t i = 0;
    while (*src && i < max_len - 1) {
        bool matched = false;
        // Общий маппинг: ищем совпадение по любой длине ключа из char_map
            for (size_t j = 0; j < sizeof(char_map)/sizeof(char_map[0]); j++) {
            size_t key_len = strlen(char_map[j].utf8);
            if (key_len == 0) continue;
            if (strncmp(src, char_map[j].utf8, key_len) == 0) {
                    dst[i++] = char_map[j].font_code;
                src += key_len;
                matched = true;
                    break;
                }
            }
        if (!matched) {
            // Однобайтовые ASCII
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

void osd_menu_init(settings_t* settings) {
    memset(&g_osd_ctx.menu, 0, sizeof(g_osd_ctx.menu));
    MENU_STATE.settings = settings;
    MENU_STATE.current_mode = MENU_MODE_MAIN;
    MENU_STATE.current_item = 0;
    for (int i = 0; i < MENU_ROWS; i++) {
        MENU_STATE.visible_items[i] = true;
    }
    osd_menu_refresh_render_buf();
}

static bool check_menu_timeout(void) {
    if (to_ms_since_boot(get_absolute_time()) - MENU_STATE.show_time > MENU_TIMEOUT_MS) {
        osd_menu_hide();
        return true;
    }
    return false;
}

// Функция определения допустимых видео режимов
static bool is_video_mode_allowed(enum video_out_mode_t mode) {
    if (!MENU_STATE.settings) return false;
    
    if (MENU_STATE.settings->manual_output_mode) return true;
    
    bool vga_connected = g_osd_ctx.vga_connected_cached;
    
    if (vga_connected) {
        // VGA кабель - только VGA режимы
        return (mode >= VGA640x480 && mode <= VGA1280x1024_d4);
    } else {
        // HDMI кабель - только DVI режим
        return (mode == DVI);
    }
}

// Функция получения следующего допустимого видео режима
static enum video_out_mode_t get_next_video_mode(enum video_out_mode_t current, bool forward) {
    enum video_out_mode_t candidate = current;
    
    for (int attempts = 0; attempts < VIDEO_MODE_MAX + 1; attempts++) {
        if (forward) {
            candidate = (candidate < VIDEO_MODE_MAX) ? candidate + 1 : VIDEO_MODE_MIN;
        } else {
            candidate = (candidate > VIDEO_MODE_MIN) ? candidate - 1 : VIDEO_MODE_MAX;
        }
        
        if (is_video_mode_allowed(candidate)) {
            return candidate;
        }
    }
    
    return current;
}

static void update_visible_items() {
    if (!MENU_STATE.settings) return;
    
    for (int i = 0; i < MENU_ROWS; i++) {
        MENU_STATE.visible_items[i] = (i <= 4) || (i >= 7);
    }
    
    if (MENU_STATE.settings->cap_sync_mode == EXT) {
        MENU_STATE.visible_items[5] = false;
        MENU_STATE.visible_items[6] = true;
    } else {
        MENU_STATE.visible_items[5] = true;
        MENU_STATE.visible_items[6] = false;
    }
}

static void prepare_menu_text() {
    if (!MENU_STATE.settings) return;

    // Меню инверсии
    if (MENU_STATE.current_mode == MENU_MODE_INVERSION) {
        MENU_STATE.items_count = INVERSION_SUBMENU_ITEMS;
        for (int i = 0; i < INVERSION_SUBMENU_ITEMS; i++) {
            char line[MENU_COLS+1] = {0};
            char line_utf8[MENU_COLS+1] = {0};
            if (i < 8) {
                snprintf(line_utf8, MENU_COLS, "%s: %d", inversion_bit_names[i], (MENU_STATE.settings->pin_inversion_mask >> (7-i)) & 1);
            } else {
                snprintf(line_utf8, MENU_COLS, "%s", inversion_bit_names[i]);
            }
            utf8_to_font_str(line_utf8, line, MENU_COLS+1);
            if (i == MENU_STATE.submenu_selection) snprintf(MENU_STATE.menu_display[i], MENU_COLS, "> %s", line);
            else snprintf(MENU_STATE.menu_display[i], MENU_COLS, "  %s", line);
        }
        osd_menu_refresh_render_buf();
        return;
    }

    // Главное меню
    if (MENU_STATE.current_mode == MENU_MODE_MAIN) {
        MENU_STATE.items_count = 4;
        for (int i = 0; i < MENU_STATE.items_count; i++) {
            utf8_to_font_str(main_menu_items[i], MENU_STATE.menu_display[i], MENU_COLS+1);
        }
        osd_menu_refresh_render_buf();
        return;
    }

    // Захват меню
    if (MENU_STATE.current_mode == MENU_MODE_CAPTURE) {
        const bool self_clk = (MENU_STATE.settings->cap_sync_mode == SELF);
        MENU_STATE.items_count = 8;
        const char* sync_mode = sync_mode_names[MENU_STATE.settings->video_sync_mode];
        const char* pixel_clock = pixel_clock_names[MENU_STATE.settings->cap_sync_mode];

        char line[MENU_COLS+1];
        char utf8buf[MENU_COLS+1];

        // 0: Синхронизация
        snprintf(utf8buf, MENU_COLS, capture_menu_items[0], sync_mode);
        utf8_to_font_str(utf8buf, line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[0], MENU_COLS, "%s", line);

        // 1: Пиксельклок
        snprintf(utf8buf, MENU_COLS, capture_menu_items[1], pixel_clock);
        utf8_to_font_str(utf8buf, line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[1], MENU_COLS, "%s", line);

        // 2: Делитель (EXT) или Частота (SELF)
        if (!self_clk) {
            snprintf(utf8buf, MENU_COLS, capture_menu_items[2], MENU_STATE.settings->ext_clk_divider);
            } else {
            snprintf(utf8buf, MENU_COLS, capture_menu_items[4], (uint32_t)(MENU_STATE.settings->frequency / 1000));
        }
        utf8_to_font_str(utf8buf, line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[2], MENU_COLS, "%s", line);

        // 3: Задержка
        snprintf(utf8buf, MENU_COLS, capture_menu_items[3], MENU_STATE.settings->delay);
        utf8_to_font_str(utf8buf, line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[3], MENU_COLS, "%s", line);

        // 4: Смещение X
        snprintf(utf8buf, MENU_COLS, capture_menu_items[5], MENU_STATE.settings->shX);
        utf8_to_font_str(utf8buf, line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[4], MENU_COLS, "%s", line);

        // 5: Смещение Y
        snprintf(utf8buf, MENU_COLS, capture_menu_items[6], MENU_STATE.settings->shY);
        utf8_to_font_str(utf8buf, line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[5], MENU_COLS, "%s", line);

        // 6: Инверсия
        utf8_to_font_str(capture_menu_items[7], line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[6], MENU_COLS, "%s", line);

        // 7: Выход
        utf8_to_font_str(capture_menu_items[8], line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[7], MENU_COLS, "%s", line);

        osd_menu_refresh_render_buf();
        return;
    }

    // Вывод меню
    if (MENU_STATE.current_mode == MENU_MODE_OUTPUT) {
        MENU_STATE.items_count = 5;
    const char* video_mode = video_mode_names[MENU_STATE.settings->video_out_mode];
    const char* scanlines_mode = MENU_STATE.settings->scanlines_mode ? "ON" : "OFF";
    const char* buffering_mode = MENU_STATE.settings->x3_buffering_mode ? "3X" : "1X";
    
    static char video_mode_display[50];
    if (MENU_STATE.settings->manual_output_mode) {
        snprintf(video_mode_display, sizeof(video_mode_display), "%s", video_mode);
    } else {
        bool vga_connected = g_osd_ctx.vga_connected_cached;
        if (vga_connected && MENU_STATE.settings->video_out_mode == DVI) {
            snprintf(video_mode_display, sizeof(video_mode_display), "%s (авто: VGA)", video_mode);
        } else if (!vga_connected && MENU_STATE.settings->video_out_mode != DVI) {
            snprintf(video_mode_display, sizeof(video_mode_display), "%s (авто: DVI)", video_mode);
        } else {
            snprintf(video_mode_display, sizeof(video_mode_display), "%s (авто)", video_mode);
        }
    }

        char line[MENU_COLS+1];
        char utf8buf[MENU_COLS+1];

        snprintf(utf8buf, MENU_COLS, output_menu_items[0], video_mode_display);
        utf8_to_font_str(utf8buf, line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[0], MENU_COLS, "%s", line);

        snprintf(utf8buf, MENU_COLS, output_menu_items[1], scanlines_mode);
        utf8_to_font_str(utf8buf, line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[1], MENU_COLS, "%s", line);

        snprintf(utf8buf, MENU_COLS, output_menu_items[2], buffering_mode);
        utf8_to_font_str(utf8buf, line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[2], MENU_COLS, "%s", line);

        snprintf(utf8buf, MENU_COLS, output_menu_items[3], MENU_STATE.settings->manual_output_mode ? "ручной" : "авто");
        utf8_to_font_str(utf8buf, line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[3], MENU_COLS, "%s", line);

        utf8_to_font_str(output_menu_items[4], line, MENU_COLS+1);
        snprintf(MENU_STATE.menu_display[4], MENU_COLS, "%s", line);

        osd_menu_refresh_render_buf();
        return;
    }
}

static void update_display(void) {
    if (!MENU_STATE.active || !MENU_STATE.settings) return;
    if (check_menu_timeout()) return;
    // Меню напрямую из menu_render_buf
}

void osd_menu_show(void) {
    if (!MENU_STATE.settings) return;

    MENU_STATE.active = true;
    MENU_STATE.current_mode = MENU_MODE_MAIN;
    MENU_STATE.current_item = 0;
    MENU_STATE.items_count = 4;
    MENU_STATE.first_visible_row = 0;
    MENU_STATE.show_time = to_ms_since_boot(get_absolute_time());
    MENU_STATE.last_button_time = MENU_STATE.show_time;

    prepare_menu_text();

    i2c_display.on = true;
    update_display();
}

void osd_menu_hide(void) {
    MENU_STATE.active = false;
    MENU_STATE.current_mode = MENU_MODE_MAIN;
//    i2c_display.on = true;
}

bool osd_menu_is_active(void) {
    return MENU_STATE.active;
}

menu_mode_t osd_menu_get_mode(void) {
    return MENU_STATE.current_mode;
}

// Функция для обработки отложенных обновлений (вызывать из основного цикла)
void osd_menu_process_pending_updates(void) {
    if (g_osd_ctx.pending_ext_clk_update && MENU_STATE.settings) {
        if (MENU_STATE.settings->cap_sync_mode == EXT) {
                set_ext_clk_divider(MENU_STATE.settings->ext_clk_divider);
        }
        g_osd_ctx.pending_ext_clk_update = false;
    }

    if (g_osd_ctx.pending_capture_restart && MENU_STATE.settings) {
        // Безопасно перезапускаем захват на core1
        stop_capture();
        start_capture(MENU_STATE.settings);
        g_osd_ctx.pending_capture_restart = false;
    }

    if (g_osd_ctx.pending_buffering_apply && MENU_STATE.settings) {
        stop_capture();
        uint8_t *old_buf = g_v_buf;
        size_t buffers = MENU_STATE.settings->x3_buffering_mode ? 3 : 1;
        uint8_t *new_buf = (uint8_t*)calloc(V_BUF_SZ * buffers, 1);
        if (new_buf) {
            g_v_buf = new_buf;
            if (old_buf) free(old_buf);
            set_v_buf_buffering_mode(MENU_STATE.settings->x3_buffering_mode);
        } else {
            // Не удалось выделить
            // Восстанавливаем захват с прежним буфером
        }
        start_capture(MENU_STATE.settings);
        g_osd_ctx.pending_buffering_apply = false;
    }
}

void osd_menu_process(uint8_t buttons) {
    if (!MENU_STATE.active) {
        if ((buttons & ENTER_BUTTON) && 
            (to_ms_since_boot(get_absolute_time()) - MENU_STATE.last_button_time > LONG_PRESS_MS)) {
            osd_menu_show();
        }
        return;
    }

    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    if (buttons != 0) {
        MENU_STATE.show_time = current_time;
    }

    if (current_time - MENU_STATE.last_button_time < DEBOUNCE_DELAY_MS) {
        return;
    }

    if (buttons != 0) {
        MENU_STATE.last_button_time = current_time;

        if (MENU_STATE.current_mode == MENU_MODE_INVERSION) {
            if (buttons & ENTER_BUTTON) {
                if (MENU_STATE.submenu_selection == 8) {
                    MENU_STATE.current_mode = MENU_MODE_CAPTURE;
                } else {
                    MENU_STATE.settings->pin_inversion_mask ^= (1 << (7 - MENU_STATE.submenu_selection));
                    apply_pin_inversion_mask(MENU_STATE.settings->pin_inversion_mask);
                }
            } 
            else if (buttons & 0x01) {
                MENU_STATE.submenu_selection = (MENU_STATE.submenu_selection > 0) ? 
                    MENU_STATE.submenu_selection - 1 : INVERSION_SUBMENU_ITEMS - 1;
            } 
            else if (buttons & 0x02) {
                MENU_STATE.submenu_selection = (MENU_STATE.submenu_selection < INVERSION_SUBMENU_ITEMS - 1) ? 
                    MENU_STATE.submenu_selection + 1 : 0;
            }
            prepare_menu_text();
            osd_menu_refresh_render_buf();
            return;
        }

        if (MENU_STATE.editing) {
            if (!(buttons & ENTER_BUTTON)) {
                bool settings_changed = false;
                
                if (MENU_STATE.current_mode == MENU_MODE_OUTPUT) {
                    switch ((output_menu_item_t)MENU_STATE.current_item) {
                        case OUTPUT_ITEM_VIDEO: {
                            if (!g_osd_ctx.vga_connected_cached) break;
                            enum video_out_mode_t new_mode = MENU_STATE.settings->video_out_mode;
                            const bool forward = (buttons & 0x02) != 0;
                            const bool backward = (buttons & 0x01) != 0;
                            if (MENU_STATE.settings->manual_output_mode) {
                                if (forward || backward) new_mode = get_next_video_mode(MENU_STATE.settings->video_out_mode, forward);
                            } else {
                                if (forward || backward) {
                                    enum video_out_mode_t candidate = MENU_STATE.settings->video_out_mode;
                                    for (int attempts = 0; attempts < VIDEO_MODE_MAX + 1; attempts++) {
                                        candidate = forward ? ((candidate < VIDEO_MODE_MAX) ? candidate + 1 : VIDEO_MODE_MIN)
                                            : ((candidate > VIDEO_MODE_MIN) ? candidate - 1 : VIDEO_MODE_MAX);
                                        if (candidate >= VGA640x480 && candidate <= VGA1280x1024_d4) { new_mode = candidate; break; }
                                    }
                                }
                            }
                            if (new_mode != MENU_STATE.settings->video_out_mode) {
                                MENU_STATE.settings->video_out_mode = new_mode;
                                g_osd_ctx.pending_video_restart = true;
                            }
                        } break;
                        case OUTPUT_ITEM_SCAN:
                            if (buttons & 0x01 || buttons & 0x02) { MENU_STATE.settings->scanlines_mode = !MENU_STATE.settings->scanlines_mode; set_scanlines_mode(); }
                        break;
                        case OUTPUT_ITEM_BUFFER:
                        if (buttons & 0x01 || buttons & 0x02) {
                            MENU_STATE.settings->x3_buffering_mode = !MENU_STATE.settings->x3_buffering_mode;
                                // Применяем на лету через отложенную последовательность на core1
                                g_osd_ctx.pending_buffering_apply = true;
                        }
                        break;
                        case OUTPUT_ITEM_MODE: {
                        if (buttons & 0x01 || buttons & 0x02) {
                                MENU_STATE.settings->manual_output_mode = !MENU_STATE.settings->manual_output_mode;
                                // На авто-режим переключимся только после сохранения (c перезапуском платы)
                                g_osd_ctx.pending_video_restart = true;
                            }
                        } break;
                        case OUTPUT_ITEM_BACK:
                        default:
                                break;
                    }
                } else if (MENU_STATE.current_mode == MENU_MODE_CAPTURE) {
                    switch ((capture_menu_item_t)MENU_STATE.current_item) {
                        case CAPTURE_ITEM_SYNC:
                            if (buttons & 0x01 || buttons & 0x02) { MENU_STATE.settings->video_sync_mode = !MENU_STATE.settings->video_sync_mode; set_video_sync_mode(MENU_STATE.settings->video_sync_mode); }
                            break;
                        case CAPTURE_ITEM_PIXCLK:
                            if (buttons & 0x01 || buttons & 0x02) {
                                MENU_STATE.settings->cap_sync_mode = (MENU_STATE.settings->cap_sync_mode == SELF) ? EXT : SELF;
                                // Откладываем перезапуск захвата, чтобы выполнить его на core1
                                g_osd_ctx.pending_capture_restart = true;
                }
                        break;
                        case CAPTURE_ITEM_DIV_OR_FREQ:
                            if (MENU_STATE.settings->cap_sync_mode == EXT) {
                                if (buttons & 0x01) { MENU_STATE.settings->ext_clk_divider = (MENU_STATE.settings->ext_clk_divider < EXT_CLK_DIVIDER_MAX) ? MENU_STATE.settings->ext_clk_divider + 1 : EXT_CLK_DIVIDER_MIN; }
                                else if (buttons & 0x02) { MENU_STATE.settings->ext_clk_divider = (MENU_STATE.settings->ext_clk_divider > EXT_CLK_DIVIDER_MIN) ? MENU_STATE.settings->ext_clk_divider - 1 : EXT_CLK_DIVIDER_MAX; }
                                // Применяем немедленно (внутри set_ext_clk_divider)
                                set_ext_clk_divider(MENU_STATE.settings->ext_clk_divider);
                            } else {
                                if (buttons & 0x01) { MENU_STATE.settings->frequency = (MENU_STATE.settings->frequency < FREQUENCY_MAX) ? MENU_STATE.settings->frequency + 1000 : FREQUENCY_MIN; }
                                else if (buttons & 0x02) { MENU_STATE.settings->frequency = (MENU_STATE.settings->frequency > FREQUENCY_MIN) ? MENU_STATE.settings->frequency - 1000 : FREQUENCY_MAX; }
                                uint16_t di; uint8_t df; calculate_clkdiv(MENU_STATE.settings->frequency, &di, &df); pio_sm_set_clkdiv_int_frac(PIO_CAP, SM_CAP, di, df); MENU_STATE.show_time = to_ms_since_boot(get_absolute_time());
                            }
                            break;
                        case CAPTURE_ITEM_DELAY:
                            if (buttons & 0x01) { MENU_STATE.settings->delay = (MENU_STATE.settings->delay < DELAY_MAX) ? MENU_STATE.settings->delay + 1 : DELAY_MIN; }
                            else if (buttons & 0x02) { MENU_STATE.settings->delay = (MENU_STATE.settings->delay > DELAY_MIN) ? MENU_STATE.settings->delay - 1 : DELAY_MAX; }
                        set_capture_delay(MENU_STATE.settings->delay);
                        break;
                        case CAPTURE_ITEM_SHX:
                            if (buttons & 0x01) { MENU_STATE.settings->shX = (MENU_STATE.settings->shX < shX_MAX) ? MENU_STATE.settings->shX + 1 : shX_MIN; }
                            else if (buttons & 0x02) { MENU_STATE.settings->shX = (MENU_STATE.settings->shX > shX_MIN) ? MENU_STATE.settings->shX - 1 : shX_MAX; }
                        set_capture_shX(MENU_STATE.settings->shX);
                        break;
                        case CAPTURE_ITEM_SHY:
                            if (buttons & 0x01) { MENU_STATE.settings->shY = (MENU_STATE.settings->shY < shY_MAX) ? MENU_STATE.settings->shY + 1 : shY_MIN; }
                            else if (buttons & 0x02) { MENU_STATE.settings->shY = (MENU_STATE.settings->shY > shY_MIN) ? MENU_STATE.settings->shY - 1 : shY_MAX; }
                        set_capture_shY(MENU_STATE.settings->shY);
                        break;
                        case CAPTURE_ITEM_INVERSION:
                        case CAPTURE_ITEM_BACK:
                        default:
                        break;
                    }
                }
                prepare_menu_text();
                osd_menu_refresh_render_buf();
            } else {
                MENU_STATE.editing = false;
                prepare_menu_text();
                osd_menu_refresh_render_buf();
            }
        } else {
            if (buttons & ENTER_BUTTON) {
                if (MENU_STATE.current_mode == MENU_MODE_MAIN) {
                    if (MENU_STATE.current_item == MAIN_ITEM_CAPTURE) { MENU_STATE.current_mode = MENU_MODE_CAPTURE; MENU_STATE.current_item = 0; MENU_STATE.editing = false; prepare_menu_text(); osd_menu_refresh_render_buf(); return; }
                    else if (MENU_STATE.current_item == MAIN_ITEM_OUTPUT) { MENU_STATE.current_mode = MENU_MODE_OUTPUT; MENU_STATE.current_item = 0; MENU_STATE.editing = false; prepare_menu_text(); osd_menu_refresh_render_buf(); return; }
                    else if (MENU_STATE.current_item == MAIN_ITEM_EXIT) { osd_menu_hide(); prepare_menu_text(); osd_menu_refresh_render_buf(); return; }
                    else if (MENU_STATE.current_item == MAIN_ITEM_SAVE) {
                        save_settings(MENU_STATE.settings);
                        if (g_osd_ctx.pending_video_restart) {
                            g_osd_ctx.pending_video_restart = false;
                            // Перезапускаем плату: при старте в авто-режиме выполнится однократный детект кабеля
                            request_restart();
                        }
                        osd_menu_hide(); prepare_menu_text(); osd_menu_refresh_render_buf(); return; }
                } else if (MENU_STATE.current_mode == MENU_MODE_CAPTURE) {
                    if (MENU_STATE.current_item == CAPTURE_ITEM_INVERSION) { MENU_STATE.current_mode = MENU_MODE_INVERSION; MENU_STATE.submenu_selection = 0; MENU_STATE.editing = false; prepare_menu_text(); osd_menu_refresh_render_buf(); return; }
                    else if (MENU_STATE.current_item == CAPTURE_ITEM_BACK) { MENU_STATE.current_mode = MENU_MODE_MAIN; MENU_STATE.current_item = 0; MENU_STATE.editing = false; prepare_menu_text(); osd_menu_refresh_render_buf(); return; }
                    else { MENU_STATE.editing = true; }
                } else if (MENU_STATE.current_mode == MENU_MODE_OUTPUT) {
                    if (MENU_STATE.current_item == OUTPUT_ITEM_BACK) { MENU_STATE.current_mode = MENU_MODE_MAIN; MENU_STATE.current_item = 0; MENU_STATE.editing = false; prepare_menu_text(); osd_menu_refresh_render_buf(); return; }
                    else { MENU_STATE.editing = true; }
                }
            } else {
                if (buttons & 0x01) { MENU_STATE.current_item = (MENU_STATE.current_item > 0) ? MENU_STATE.current_item - 1 : (MENU_STATE.items_count - 1); }
                else if (buttons & 0x02) { MENU_STATE.current_item = (MENU_STATE.current_item + 1) % MENU_STATE.items_count; }
            }
            prepare_menu_text();
            osd_menu_refresh_render_buf();
        }
    }

    update_display();
}