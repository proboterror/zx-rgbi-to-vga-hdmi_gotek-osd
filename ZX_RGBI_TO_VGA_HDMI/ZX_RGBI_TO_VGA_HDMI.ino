#include <Arduino.h>

#include "pico.h"
#include "pico/time.h"
#include "hardware/vreg.h"

#include "serial_menu.h"

extern "C"
{
#include "g_config.h"
#include "rgb_capture.h"
#include "settings.h"
#include "v_buf.h"
#include "video_output.h"
#include "gotek_i2c_osd.h"
#include "ps2_keyboard.h"
#include "zx_keyboard.h"

#ifdef OSD_MENU_ENABLE
#include "osd_menu.h"
#endif
}

// Default settings values for Deltagon 1.6+
settings_t settings = {
  .video_out_type = DVI,
  .video_out_mode = MODE_720x576_50Hz,
  .scanlines_mode = false,
  .buffering_mode = false,
  .video_sync_mode = true,
  .cap_sync_mode = EXT,
  .frequency = 7000000,
  .ext_clk_divider = 1,
  .delay = 20,
  .shX = 137,
  .shY = 40,
  .pin_inversion_mask = 0b01011111,
};

volatile bool start_core0 = false;

volatile bool stop_core1 = false;
volatile bool core1_inactive = false;

volatile bool restart_capture = false;
volatile bool capture_active = false;

void setup()
{
  vreg_set_voltage(VREG_VOLTAGE_1_25);
  sleep_ms(100);

  setup_i2c_slave();
#ifndef WAVESHARE_RP2040_ZERO
  zx_keyboard_init();
  ps2_keyboard_init();
#endif
  Serial.begin(9600);

  load_settings(&settings);
  set_buffering_mode(settings.buffering_mode);
  draw_welcome_screen(*(video_modes[settings.video_out_mode]));
  set_scanlines_mode();
  start_video_output(settings.video_out_type);

#ifdef OSD_MENU_ENABLE
  osd_init();
#endif

  start_core0 = true;

  Serial.println("  Starting...\n");
}

void loop()
{
#ifdef OSD_MENU_ENABLE
  osd_update();

  if (!osd_state.visible)
  {
#endif
    char c = get_menu_input(100);

    if (c != 0)
      handle_serial_menu();

#ifdef OSD_MENU_ENABLE
  }
#endif
}

void setup1()
{
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  while (!start_core0)
    sleep_ms(10);

  start_capture();
}

void __not_in_flash_func(loop1())
{
  uint32_t frame_count_tmp1 = frame_count;

  sleep_ms(100);

  if (frame_count > 1)
  {
    digitalWrite(PIN_LED, (frame_count & 0x20) && capture_active);

    if (frame_count == frame_count_tmp1)
    {
      if (capture_active)
      {
        capture_active = false;
        draw_no_signal(*(video_modes[settings.video_out_mode]));
      }
    }
    else if (!capture_active)
      capture_active = true;
  }

  if (restart_capture)
  {
    stop_capture();
    start_capture();
    restart_capture = false;
  }

  if (stop_core1)
  {
    core1_inactive = true;

    uint32_t ints = save_and_disable_interrupts();

    while (core1_inactive)
      ;

    restore_interrupts_from_disabled(ints);
  }

  osd_process();
#ifndef WAVESHARE_RP2040_ZERO
  zx_keyboard_update();
#endif
}