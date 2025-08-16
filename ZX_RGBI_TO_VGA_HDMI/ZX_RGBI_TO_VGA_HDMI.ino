#include "osd_menu_wrapper.h"
#include <Arduino.h>
#include <typeinfo>
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h" // Автоматически генерируется из .pio файла
#include "pico/usb_reset_interface.h"
#include "hardware/structs/usb.h"
#include <stdarg.h>

// Внешние C-библиотеки
extern "C" {
    #include "g_config.h"          // Конфигурация системы
    #include "pico/stdlib.h"       // Стандартные функции Pico SDK
    #include "hardware/vreg.h"     // Управление напряжением
    #include "gotek_i2c_osd.h"     // OSD (On Screen Display)
    #include "ps2_keyboard.h"      // Поддержка PS2 клавиатуры
    #include "zx_keyboard.h"       // Поддержка клавиатуры ZX Spectrum
    #include "hardware/flash.h"    // Работа с flash-памятью
    #include "hardware/watchdog.h" // Watchdog таймер
    #include "rgb_capture.h"       // Захват RGB видео
    #include "stdio.h"             // Стандартный ввод/вывод
    #include "v_buf.h"             // Видеобуфер
    #include "vga.h"               // VGA вывод
    #include "dvi.h"               // DVI вывод
}

#define LED_PIN 16
#define LED_COUNT 1
#define RESET_PIN 28

#define BUTTON_DEBOUNCE_MS 50    // Время антидребезга
#define BUTTON_LONG_PRESS_MS 3000 // Время длинного нажатия (3 секунды)

// Цвета определены в g_config.h

// Определения для удобства
#define printf Serial.printf

// Настройки по умолчанию
const settings_t DEFAULT_SETTINGS = {
  .video_out_mode = VGA640x480,    // Режим вывода видео (VGA 640x480)
  .scanlines_mode = false,         // Режим scanlines (эмуляция ЭЛТ)
  .x3_buffering_mode = false,      // Тройная буферизация
  .video_sync_mode = true,         // Синхронизация видео
  .cap_sync_mode = EXT,            // Режим синхронизации (внешний)
  .frequency = 7000000,            // Частота захвата (7 МГц)
  .ext_clk_divider = 2,            // Делитель внешней частоты
  .delay = 17,                     // Задержка сигнала
  .shX = 137,                      // Горизонтальное смещение
  .shY = 40,                       // Вертикальное смещение
  .pin_inversion_mask = 0b10100000, // Маска инверсии пинов
  .manual_output_mode = false      // По умолчанию автоопределение
};

settings_t settings = DEFAULT_SETTINGS; // Активные настройки

void log_message(const char* format, ...) {
    if (!Serial) return;
    
    // Проверяем размер буфера для предотвращения переполнения
    va_list args;
    va_start(args, format);
    
    // Используем временный буфер достаточного размера
    char buffer[256];
    int written = vsnprintf(buffer, sizeof(buffer), format, args);
    
    // Проверяем успешность форматирования
    if (written > 0 && written < (int)sizeof(buffer)) {
        Serial.print(buffer);
    } else {
        Serial.print("Ошибка форматирования лога\n");
    }
    
    va_end(args);
}

void safe_restart() {
    // Даем время на сброс
    sleep_ms(100);
    // Полная перезагрузка
    rp2040.restart();
}

// Нежесткий запрос рестарта из другого контекста (OSD)
static volatile bool g_restart_requested = false;
extern "C" void request_restart(void) {
    g_restart_requested = true;
}

extern "C" bool is_vga_cable_connected(void) {
    // 1. Выбираем один из выходных пинов VGA 
    const uint vga_pin = VGA_PIN_D0;
    
    // Проверяем валидность пина
    if (vga_pin >= 30) { // RP2040 имеет 30 GPIO пинов
        printf("Ошибка: недопустимый VGA пин %d\n", vga_pin);
        return false;
    }
    
    // 2. Сохраняем текущее состояние пина
    gpio_set_dir(vga_pin, GPIO_IN);
    bool orig_pull = gpio_is_pulled_up(vga_pin);
    gpio_set_pulls(vga_pin, false, true); // Включаем PULLDOWN
    
    // 3. Подаем тестовый импульс
    gpio_set_dir(vga_pin, GPIO_OUT);
    gpio_put(vga_pin, 1);
    busy_wait_us(1); // Короткий импульс 1 мкс
    
    // 4. Проверяем скорость разряда
    gpio_set_dir(vga_pin, GPIO_IN);
    uint32_t start = time_us_32();
    uint32_t timeout_counter = 0;
    const uint32_t max_timeout = 10; // Таймаут 10 мкс
    
    while(gpio_get(vga_pin) && timeout_counter < max_timeout) {
        timeout_counter++;
        busy_wait_us(1);
    }
    uint32_t discharge_time = timeout_counter;
    
    printf("Discharge time: %d μs\n", discharge_time);
    
    // 5. Восстанавливаем состояние
    gpio_set_pulls(vga_pin, orig_pull, !orig_pull);
    
    // 6. Анализ результатов
    // С 75 Ом нагрузкой разряд будет быстрее
    return (discharge_time < 5); // Эмпирическое значение, требует калибровки
}

// Минимальный драйвер WS2812 (1 светодиод)
static bool g_ws_inited = false;
static PIO g_ws_pio = pio1;
static uint g_ws_sm = 1;
static uint g_ws_offset = 0;

// Инициализация PIO для WS2812
bool neopixel_init() {
    if (g_ws_inited) return true;
    // Загружаем программу в PIO1, используем SM1 (SM0 занят захватом)
    g_ws_offset = pio_add_program(g_ws_pio, &ws2812_program);
    ws2812_program_init(g_ws_pio, g_ws_sm, g_ws_offset, LED_PIN, 800000.0f, false);
    g_ws_inited = true;
    return true;
}

// Установка цвета (GRB)
void neopixel_set_color(uint32_t color) {
    if (!g_ws_inited) return;
    pio_sm_put_blocking(g_ws_pio, g_ws_sm, (color << 8));
}

extern "C" void set_led(bool state, uint32_t color /*= LED_GREEN*/) {
    if (!g_ws_inited) {
        if (!neopixel_init()) return;
    }
    neopixel_set_color(state ? color : 0);
}

void check_button() {
    static uint32_t press_time = 0;
    static bool pressed = false;
    static uint32_t last_check = 0;
    
    // Ограничиваем частоту проверки для экономии ресурсов
    uint32_t current_time = millis();
    if (current_time - last_check < 10) return;
    last_check = current_time;
    
    bool btn_state = digitalRead(RESET_PIN);

    if (!pressed && !btn_state) {
        pressed = true;
        press_time = current_time;
        return;
    }

    if (pressed && btn_state) {
        pressed = false;
        uint32_t duration = current_time - press_time;
        
        // Проверяем минимальное время нажатия для антидребезга
        if (duration > BUTTON_DEBOUNCE_MS) {
            // Для кнопки меню используем только короткие нажатия
            if (duration < BUTTON_LONG_PRESS_MS) {
                osd_menu_process(ENTER_BUTTON);
            }
        }
    }
}

// для кнопок навигации
uint8_t check_navigation_buttons() {
    static uint32_t up_press_time = 0, down_press_time = 0;
    static bool up_pressed = false, down_pressed = false;
    static uint32_t last_up_release = 0, last_down_release = 0;
    static uint32_t last_check = 0;
    
    // Ограничиваем частоту проверки для экономии ресурсов
    uint32_t current_time = millis();
    if (current_time - last_check < 10) return 0;
    last_check = current_time;
    
    bool up_state = digitalRead(26);
    bool down_state = digitalRead(27);
    uint8_t buttons = 0;
    
    // UP button edge detection
    if (!up_pressed && !up_state) {
        up_pressed = true;
        up_press_time = current_time;
    } else if (up_pressed && up_state) {
        up_pressed = false;
        uint32_t duration = current_time - up_press_time;
        // Проверяем антидребезг и короткое нажатие
        if (duration > 20 && duration < 1000 && (current_time - last_up_release > 100)) {
            buttons |= 0x01; // UP
            last_up_release = current_time;
        }
    }
    
    // DOWN button edge detection
    if (!down_pressed && !down_state) {
        down_pressed = true;
        down_press_time = current_time;
    } else if (down_pressed && down_state) {
        down_pressed = false;
        uint32_t duration = current_time - down_press_time;
        // Проверяем антидребезг и короткое нажатие
        if (duration > 20 && duration < 1000 && (current_time - last_down_release > 100)) {
            buttons |= 0x02; // DOWN
            last_down_release = current_time;
        }
    }
    
    return buttons;
}

uint16_t Fletcher16(uint8_t *data, int count) {
  uint16_t sum1 = 0;
  uint16_t sum2 = 0;
  int index;

  for (index = 0; index < count; ++index) {
    sum1 = (sum1 + data[index]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }

  return (sum2 << 8) | sum1;
}

struct flash_settings_t {
  settings_t settings;
  uint16_t checksum;
};

const int *saved_settings = (const int *)(XIP_BASE + (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE));
bool is_start_core0 = false;

extern "C" void save_settings(settings_t *settings) {
  static flash_settings_t flash_settings_buffer;
  
  flash_settings_buffer = {
    *settings,
    Fletcher16((uint8_t*)settings, sizeof(settings_t))
  };

  rp2040.idleOtherCore();
  uint32_t ints = save_and_disable_interrupts();

  flash_range_erase((PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE), FLASH_SECTOR_SIZE);
  flash_range_program((PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE), (uint8_t *)&flash_settings_buffer, FLASH_PAGE_SIZE);

  restore_interrupts(ints);
  rp2040.resumeOtherCore();
}

void set_scanlines_mode() {
  if (settings.video_out_mode != DVI)
    set_vga_scanlines_mode(settings.scanlines_mode);
}

void setup() {

    setup_i2c_slave(); 
    pinMode(RESET_PIN, INPUT_PULLUP);
    vreg_set_voltage(VREG_VOLTAGE_1_25);
    sleep_ms(100);

    set_sys_clock_khz(252000, true);
    sleep_ms(10);

    Serial.begin(115200);
    

    // Добавьте задержку перед загрузкой настроек
    sleep_ms(500);  // Даем время на стабилизацию питания
    
#ifndef WAVESHARE_RP2040_ZERO
    zx_keyboard_init();
    ps2_keyboard_init();
#endif

    // Загрузка сохраненных настроек
    const flash_settings_t *flash_settings = (flash_settings_t*)saved_settings;
    const uint16_t checksum = Fletcher16((uint8_t*)saved_settings, sizeof(settings_t));
    
    if(checksum == flash_settings->checksum) {
        memcpy(&settings, saved_settings, sizeof(settings_t));
//        printf("Настройки загружены из flash\n");
    } else {
        settings = DEFAULT_SETTINGS;
//        printf("Используются настройки по умолчанию\n");
    }
        
    check_settings(&settings);

    // Режим конфигурации через Serial
    if (watchdog_caused_reboot()) {
        char s_key[20];
        int s_data;
        bool is_save = false;
        
        delay(3000);  // Даем время на открытие порта
        
        if (Serial) {
            
            while(1) {
                if (Serial.available()) {
                    String s1 = Serial.readStringUntil('\n');
                    if (s1.length() == 0) continue;
                
                    // Проверяем длину строки для безопасности
                    if (s1.length() > 50) {
//                        printf("Ошибка: слишком длинная команда\n");
                        continue;
                    }
                    
                    char s_key[20];
                    int s_data;
                    int parsed = sscanf(s1.c_str(), "%19s%d", s_key, &s_data);
                    
                    // Проверяем успешность парсинга
                    if (parsed < 1) {
//                        printf("Ошибка: неверный формат команды\n");
                        continue;
                    }

                    if (strcmp(s_key, "ping") == 0) { printf("ping ok\n"); continue; }
                    if (strcmp(s_key, "mode") == 0) { printf("mode 0\n"); continue; }
                    if (strcmp(s_key, "exit") == 0) { printf("exit ok\n"); break; }
                    if (strcmp(s_key, "save") == 0) { is_save = true; printf("saving...\n"); break; }
                
                    #define CAP_SET_LOAD(x,T) { \
                        if(s_key[0] == 'r' || s_key[0] == 'w') { \
                            if(s_key[0] == 'w') { \
                                (x) = static_cast<T>(s_data); \
                                /* При ручном изменении video_out_mode включаем ручной режим */ \
                                if(strcmp(s_key+1, "video_out_mode") == 0) { \
                                    settings.manual_output_mode = true; \
                                } \
                            } \
                            check_settings(&settings); \
                            printf("%s %d\n", s_key, (x)); \
                            continue; \
                        } \
                    };
                                
                    if (strcmp(s_key+1, "video_out_mode") == 0) CAP_SET_LOAD(settings.video_out_mode, video_out_mode_t)
                    if (strcmp(s_key+1, "scanlines_mode") == 0) CAP_SET_LOAD(settings.scanlines_mode, int)
                    if (strcmp(s_key+1, "x3_buffering_mode") == 0) CAP_SET_LOAD(settings.x3_buffering_mode, int)
                    if (strcmp(s_key+1, "video_sync_mode") == 0) CAP_SET_LOAD(settings.video_sync_mode, int)
                    if (strcmp(s_key+1, "cap_sync_mode")==0) CAP_SET_LOAD(settings.cap_sync_mode,cap_sync_mode_t) 
                    if (strcmp(s_key+1, "frequency") == 0) CAP_SET_LOAD(settings.frequency, int)
                    if (strcmp(s_key+1, "ext_clk_divider") == 0) CAP_SET_LOAD(settings.ext_clk_divider, int)
                    if (strcmp(s_key+1, "delay") == 0) CAP_SET_LOAD(settings.delay, int)
                    if (strcmp(s_key+1, "cap_sh_x") == 0) CAP_SET_LOAD(settings.shX, int)
                    if (strcmp(s_key+1, "cap_sh_y") == 0) CAP_SET_LOAD(settings.shY, int)
                    if (strcmp(s_key+1, "pin_inversion_mask") == 0) CAP_SET_LOAD(settings.pin_inversion_mask, int)

                    printf("wrong command\n");
                }
            }

            if (is_save) {
                save_settings(&settings);
                printf("saving data\n");
            }
        }
    }
    else {
    // Пропускаем конфигурацию и сразу запускаем видео
    }

    // Инициализация OSD меню
    osd_menu_init(&settings);
    
    // Инициализация пинов кнопок навигации меню
    gpio_init(26); // UP
    gpio_set_dir(26, GPIO_IN);
    gpio_pull_up(26);
    
    gpio_init(27); // DOWN
    gpio_set_dir(27, GPIO_IN);
    gpio_pull_up(27);

    // Проверка подключения кабеля один раз при старте и сохранение результата для меню
    const bool vga_connected_once = is_vga_cable_connected();
    osd_menu_set_vga_connected(vga_connected_once);

    // Используем кэш при первичной настройке видео в авто-режиме
    if (vga_connected_once) {
        if (!settings.manual_output_mode) {
            settings.video_out_mode = VGA640x480;
            printf("VGA cable detected (auto mode)\n");
        } else {
            printf("Manual output mode active, using saved settings\n");
        }
    } else {
        if (!settings.manual_output_mode) {
            settings.video_out_mode = DVI;
            printf("HDMI/DVI cable detected (auto mode)\n");
        } else {
            printf("Manual output mode active, using saved settings\n");
        }
    }

    // Еще небольшая задержка перед запуском видео
    sleep_ms(100);

    // Выделяем видеобуфер ПОСЛЕ выбора режима
    // Всегда пере-выделяем буфер на нужное количество страниц при старте,
    // чтобы исключить рассинхронизацию после перезапуска
    if (g_v_buf) { free(g_v_buf); g_v_buf = NULL; }
    {
        size_t buffers = settings.x3_buffering_mode ? 3 : 1;
        g_v_buf = (uint8_t*)calloc(V_BUF_SZ * buffers, 1);
        if (!g_v_buf && buffers == 3) {
            g_v_buf = (uint8_t*)calloc(V_BUF_SZ, 1);
            settings.x3_buffering_mode = false;
        }
    }
    // Инициализируем состояние буферов согласно актуальному режиму
    set_v_buf_buffering_mode(settings.x3_buffering_mode);

    set_scanlines_mode();

    if (settings.video_out_mode == DVI) {
        start_dvi(*(vga_modes[settings.video_out_mode]));
    } else {
        start_vga(*(vga_modes[settings.video_out_mode]));
    }

    is_start_core0 = true;
}

void loop() {
    static uint32_t last_log = 0;
    static uint32_t last_button_check = 0;
    static uint32_t last_serial_check = 0;
    uint32_t current_time = millis();

    // Логирование каждую секунду
    if (current_time - last_log > 1000) {
        last_log = current_time;
//        log_message("System alive - Menu active: %s\n", osd_menu_is_active() ? "YES" : "NO");
    }
    
    // Проверка кнопки каждые 10мс
    if (current_time - last_button_check >= 10) {
        last_button_check = current_time;
        check_button();
    }

    // Проверка Serial каждые 50мс
    if (current_time - last_serial_check >= 50) {
        last_serial_check = current_time;
        if (Serial.available()) {
            String s1 = Serial.readStringUntil('\n');
            if (s1.length() > 0) {
                // Проверяем длину строки для безопасности
                if (s1.length() > 50) {
 //                   printf("Ошибка: слишком длинная команда\n");
                    return;
                }
                
                char s_key[20];
                int s_data;
                int parsed = sscanf(s1.c_str(), "%19s%d", s_key, &s_data);
                
                // Проверяем успешность парсинга
                if (parsed < 1) {
//                    printf("Ошибка: неверный формат команды\n");
                    return;
                }

                if (strcmp(s_key, "ping") == 0) {
                    printf("ping ok\n");
                } 
                else if (strcmp(s_key, "reset") == 0 || strcmp(s_key, "restart") == 0) {
                    printf("reset...\n");
                    safe_restart();
                }
                else if (strcmp(s_key, "mode") == 0) {
                    printf("mode 1\n");
                }
                else if (strcmp(s_key, "print") == 0) {
                    printf("Current settings:\n");
                    printf("video_out_mode: %d\n", settings.video_out_mode);
                    printf("manual_output_mode: %d\n", settings.manual_output_mode);
                    printf("scanlines_mode: %d\n", settings.scanlines_mode);
                    printf("x3_buffering_mode: %d\n", settings.x3_buffering_mode);
                    printf("video_sync_mode: %d\n", settings.video_sync_mode);
                    printf("cap_sync_mode: %d\n", settings.cap_sync_mode);
                    printf("frequency: %d\n", settings.frequency);
                    printf("ext_clk_divider: %d\n", settings.ext_clk_divider);
                    printf("delay: %d\n", settings.delay);
                    printf("shX: %d\n", settings.shX);
                    printf("shY: %d\n", settings.shY);
                    printf("pin_inversion_mask: 0b");
                    for(int i = 7; i >= 0; i--) {
                        printf("%d", (settings.pin_inversion_mask >> i) & 1);
                        if(i == 4) printf(" ");
                    }
                    printf("\n");
                }
                else if (strcmp(s_key+1, "cap_sh_x") == 0) {
                    if(s_key[0] == 'w') set_capture_shX(s_data);
                }
                else if (strcmp(s_key+1, "cap_sh_y") == 0) {
                    if(s_key[0] == 'w') set_capture_shY(s_data);
                }
                else if (strcmp(s_key+1, "delay") == 0) {
                    if(s_key[0] == 'w') set_capture_delay(s_data);
                }
                else {
                    printf("Unknown command: %s\n", s_key);
                }
            }
        }
    }

    // Увеличиваем задержку для снижения нагрузки на CPU
    delay(5);
}

void setup1() {
    while(!is_start_core0) sleep_ms(3);
    sleep_ms(300);  // Доп. задержка для стабилизации
    start_capture(&settings);
}


void loop1() {
    static uint32_t last_button_check = 0;
    static uint32_t last_keyboard_update = 0;
    uint32_t current_time = millis();
 
    while(true) {
        current_time = millis();
        if (g_restart_requested) {
            g_restart_requested = false;
            safe_restart();
        }
        
        // Проверка кнопок навигации каждые 10мс
        if (current_time - last_button_check >= 10) {
            last_button_check = current_time;
            uint8_t buttons = check_navigation_buttons();
     
            if (osd_menu_is_active()) {
               osd_menu_process(buttons);
            }
            
            uint8_t inv0_mask = settings.pin_inversion_mask;
            if (bitRead(inv0_mask, 7)) {
                osd_process();
            } else {
                // Явно отключаем OSD, если 7-й бит сброшен
                i2c_display.on = false;
            }
            
            osd_menu_process_pending_updates();
        }
        
        // Обновление клавиатуры каждые 20мс
        if (current_time - last_keyboard_update >= 20) {
            last_keyboard_update = current_time;
        #ifndef WAVESHARE_RP2040_ZERO
            zx_keyboard_update();
        #endif
        }
        
        sleep_ms(5);
    }
}