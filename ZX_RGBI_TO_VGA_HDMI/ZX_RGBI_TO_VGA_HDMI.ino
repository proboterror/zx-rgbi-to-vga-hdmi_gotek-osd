#include <Arduino.h>
#include <typeinfo>
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h" // Автоматически генерируется из .pio файла
#include "pico/usb_reset_interface.h"
#include "hardware/structs/usb.h"

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
    #include "VGA.h"               // VGA вывод
    #include "DVI.h"               // DVI вывод
}

#define LED_PIN 16
#define LED_COUNT 1
#define RESET_PIN 28

#define BUTTON_DEBOUNCE_MS 50    // Время антидребезга
#define BUTTON_LONG_PRESS_MS 3000 // Время длинного нажатия (3 секунды)
#define LED_RESET_BLINK_INTERVAL 200 // Интервал мигания при сбросе

// Цвета для светодиода (в формате GRB для WS2812)
#define LED_OFF     0x000000
#define LED_RED     0x00FF00  // Было 0xFF0000
#define LED_GREEN   0xFF0000  // Было 0x00FF00
#define LED_BLUE    0x0000FF
#define LED_YELLOW  0xFFFF00  // Остается таким же (R+G)
#define LED_CYAN    0x00FFFF  // Остается таким же (G+B)
#define LED_MAGENTA 0xFF00FF  // Остается таким же (R+B)
#define LED_WHITE   0xFFFFFF

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

void safe_restart() {
    // Даем время на сброс
    sleep_ms(100);
    // Полная перезагрузка
    rp2040.restart();
}

bool is_vga_cable_connected() {
    // 1. Выбираем один из выходных пинов VGA 
    const uint vga_pin = VGA_PIN_D0;
    
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
    while(gpio_get(vga_pin)) {
        if(time_us_32() - start > 10) break; // Таймаут 10 мкс
    }
    uint32_t discharge_time = time_us_32() - start;
    
    printf("Discharge time: %d μs\n", discharge_time);
    // 5. Восстанавливаем состояние
    gpio_set_pulls(vga_pin, orig_pull, !orig_pull);
    
    // 6. Анализ результатов
    // С 75 Ом нагрузкой разряд будет быстрее
    return (discharge_time < 5); // Эмпирическое значение, требует калибровки
}

// Инициализация PIO для WS2812
void neopixel_init() {
    PIO pio = pio1; // Используем PIO1
    int sm = 1;     // Используем SM1 (было SM0)
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, LED_PIN, 800000, false);
    printf("WS2812 на PIO%d, SM%d\n", pio == pio0 ? 0 : 1, sm);
}

// Установка цвета (не блокирует ядро!)
void neopixel_set_color(uint32_t color) {
    pio_sm_put_blocking(pio1, 1, color << 8u); // Используем SM1
}

void set_led(bool state, uint32_t color = LED_GREEN) {
#ifdef WAVESHARE_RP2040_ZERO
    static bool led_state = false;
    static uint32_t led_color = LED_GREEN;  // По умолчанию зеленый

    led_state = state;
    if (color != 0) led_color = color;

    if (!state) {
        neopixel_set_color(LED_OFF);
    } else {
        neopixel_set_color(led_color);
    }
#else
    digitalWrite(LED_BUILTIN, state ? HIGH : LOW);
#endif
}

void check_button() {
    static uint32_t press_start_time = 0;
    static bool button_pressed = false;
    static bool long_press_handled = false;
    static bool reset_in_progress = false;
    static uint32_t last_blink_time = 0;
    uint32_t current_time = millis();

    bool current_state = digitalRead(RESET_PIN);

    // Обработка сброса (мигание светодиодом)
    if (reset_in_progress) {
        if (current_time - last_blink_time >= LED_RESET_BLINK_INTERVAL) {
            set_led(!digitalRead(LED_PIN), LED_RED); // Мигаем красным
            last_blink_time = current_time;
        }
        return;
    }

    // Обработка нажатия
    if (!button_pressed && current_state == LOW) {
        // Начало нажатия
        button_pressed = true;
        press_start_time = current_time;
        long_press_handled = false;
        return;
    }

    if (button_pressed && current_state == HIGH) {
        // Кнопка отпущена
        button_pressed = false;
        
        // Короткое нажатие (если длинное не было обработано)
        if (!long_press_handled && (current_time - press_start_time < BUTTON_LONG_PRESS_MS)) {
            // Инвертируем 7-й бит и обновляем состояние OSD
            settings.pin_inversion_mask ^= (1 << 7);
            i2c_display.on = (settings.pin_inversion_mask & (1 << 7)) != 0;
            
            if (settings.pin_inversion_mask & (1 << 7)) {
                set_led(true, LED_GREEN);
            } else {
                set_led(true, LED_YELLOW);
            }
        }
        return;
    }

    // Обработка длинного нажатия
    if (button_pressed && !long_press_handled && 
        (current_time - press_start_time >= BUTTON_LONG_PRESS_MS)) {
        
        long_press_handled = true;
        reset_in_progress = true;
        last_blink_time = current_time;
        
        // Сброс к заводским настройкам
        printf("default...\n");
        settings = DEFAULT_SETTINGS;
        save_settings(&settings);
        
        // Мигаем красным 3 раза
        for (int i = 0; i < 3; i++) {
            set_led(true, LED_RED);
            delay(200);
            set_led(false, LED_RED);
            delay(200);
        }
        
        // Перезагрузка
        printf("Reboot...\n");
        delay(100);
        rp2040.restart();
    }
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

void save_settings(settings_t *settings) {
  flash_settings_t* flash_settings = (flash_settings_t*)malloc(FLASH_PAGE_SIZE);

  *flash_settings = {
    *settings,
    Fletcher16((uint8_t*)settings, sizeof(settings_t))
  };

  rp2040.idleOtherCore();
  uint32_t ints = save_and_disable_interrupts();

  flash_range_erase((PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE), FLASH_SECTOR_SIZE);
  flash_range_program((PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE), (uint8_t *)flash_settings, FLASH_PAGE_SIZE);

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
    
    neopixel_init();
    set_led(true, LED_CYAN);  // Временный индикатор загрузки

    // Добавьте задержку перед загрузкой настроек
    sleep_ms(500);  // Даем время на стабилизацию питания
    
    // Инициализация светодиода в зависимости от режима
    if (watchdog_caused_reboot() && Serial) {
        // Режим настройки - фиолетовый
        set_led(true, LED_MAGENTA);
    } else {
        // Нормальный режим - зеленый
        if (settings.pin_inversion_mask & (1 << 7)) {
            set_led(true, LED_GREEN);
        } else {
            set_led(true, LED_YELLOW);
        }
    }
    

#ifndef WAVESHARE_RP2040_ZERO
    zx_keyboard_init();
    ps2_keyboard_init();
#endif

    // Загрузка сохраненных настроек
    const flash_settings_t *flash_settings = (flash_settings_t*)saved_settings;
    const uint16_t checksum = Fletcher16((uint8_t*)saved_settings, sizeof(settings_t));
    
    if(checksum == flash_settings->checksum) {
        memcpy(&settings, saved_settings, sizeof(settings_t));
    } else {
        settings = DEFAULT_SETTINGS;
    }
        
    check_settings(&settings);

    // Режим конфигурации через Serial
    if (watchdog_caused_reboot()) {
        char s_key[20];
        int s_data;
        bool is_save = false;
        
        delay(3000);  // Даем время на открытие порта
        
        if (Serial) {
            set_led(true, LED_MAGENTA);  // Фиолетовый в режиме настройки
            
            while(1) {
                if (Serial.available()) {
                    String s1 = Serial.readStringUntil('\n');
                    if (s1.length() == 0) continue;
                
                    sscanf(s1.c_str(), "%19s%d", s_key, &s_data);

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

    // Проверка подключения кабеля с учетом режима
    if (is_vga_cable_connected()) {
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

    set_v_buf_buffering_mode(settings.x3_buffering_mode);
    draw_welcome_screen(*(vga_modes[settings.video_out_mode]));

    set_scanlines_mode();

    if (settings.video_out_mode == DVI) {
        start_dvi(*(vga_modes[settings.video_out_mode]));
    } else {
        start_vga(*(vga_modes[settings.video_out_mode]));
    }

    // Установка финального цвета светодиода
    if (Serial && Serial.available()) {
        set_led(true, LED_MAGENTA);  // Фиолетовый в режиме настройки
    } else {
        if (settings.pin_inversion_mask & (1 << 7)) {
            set_led(true, LED_GREEN);
        } else {
            set_led(true, LED_YELLOW);
        }
    }

    is_start_core0 = true;
}

void loop() {
    static uint32_t last_button_check = 0;
    static uint32_t last_serial_check = 0;
    static uint32_t last_led_update = 0;
    uint32_t current_time = millis();

    //if (current_time - last_button_check >= 10) {
    //    last_button_check = current_time;
        check_button();
    //}

    if (current_time - last_serial_check >= 50) {
        last_serial_check = current_time;
        

        // Обновляем цвет светодиода при изменении состояния Serial
        static bool last_serial_state = false;
        bool current_serial_state = (Serial && Serial.available());
        if (current_serial_state != last_serial_state) {
            if (current_serial_state) {
                set_led(true, LED_MAGENTA);  // Фиолетовый в режиме настройки
            } else {
                if (settings.pin_inversion_mask & (1 << 7)) {
                    set_led(true, LED_GREEN);
                } else {
                set_led(true, LED_YELLOW);
                }
            }
            last_serial_state = current_serial_state;
        }

        if (Serial.available()) {
            String s1 = Serial.readStringUntil('\n');
            if (s1.length() > 0) {
                char s_key[20];
                int s_data;
                sscanf(s1.c_str(), "%19s%d", s_key, &s_data);
                
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

    if (current_time - last_led_update >= 100) {
        last_led_update = current_time;
    }

    delay(1);
}

void setup1() {
    while(!is_start_core0) sleep_ms(3);
    sleep_ms(300);  // Доп. задержка для стабилизации
    start_capture(&settings);
}

void loop1() {
    uint8_t inv0_mask = settings.pin_inversion_mask;
    if (bitRead(inv0_mask, 7)) {
        osd_process();
    } else {
        // Явно отключаем OSD, если 7-й бит сброшен
        i2c_display.on = false;
    }
#ifndef WAVESHARE_RP2040_ZERO
    zx_keyboard_update();
#endif
    sleep_ms(1);
}