#include "g_config.h"
#include "dvi.h"
#include "pio_programs.h"
#include "v_buf.h"

#include "gotek_i2c_osd.h"
#include "font.h"
#include "osd_menu_wrapper.h" 

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/systick.h"
#include "hardware/vreg.h"

static int dma_ch1;
static uint8_t *screen_buf;
static video_mode_t video_mode;

static int16_t h_visible_area;

// the number of DMA buffers can be increased if there is image fluttering
static uint32_t *v_out_dma_buf[2];
static uint32_t *v_out_dma_buf_addr[2];
static uint64_t sync_data[4];
static uint64_t R64, G64, B64, Y64;
static uint64_t palette[32];

static void __not_in_flash_func(memset64)(uint64_t *dst, const uint64_t data, uint32_t size)
{
  dst[0] = data;

  for (int i = 1; i < size; i++)
    *++dst = data;
}

static uint64_t get_ser_diff_data(uint16_t dataR, uint16_t dataG, uint16_t dataB)
{
  uint64_t out64 = 0;
  uint8_t d6;
  uint8_t bR;
  uint8_t bG;
  uint8_t bB;

  for (int i = 0; i < 10; i++)
  {
    out64 <<= 6;

    if (i == 5)
      out64 <<= 2;

    bR = (dataR >> (9 - i)) & 1;
    bG = (dataG >> (9 - i)) & 1;
    bB = (dataB >> (9 - i)) & 1;

    bR |= (bR ^ 1) << 1;
    bG |= (bG ^ 1) << 1;
    bB |= (bB ^ 1) << 1;

    if (DVI_PIN_invert_diffpairs)
    {
      bR ^= 0b11;
      bG ^= 0b11;
      bB ^= 0b11;
    }

    if (DVI_PIN_RGB_notBGR)
    {
      d6 = (bR << 4) | (bG << 2) | (bB << 0);
    }
    else
    {
      d6 = (bB << 4) | (bG << 2) | (bR << 0);
    }

    out64 |= d6;
  }

  return out64;
}

// TMDS encoder
static uint tmds_encoder(uint8_t d8)
{
  int s1 = 0;
  uint8_t xnor = 0;

  for (int i = 0; i < 8; i++)
    s1 += (d8 & (1u << i)) ? 1 : 0;

  if ((s1 > 4) || ((s1 == 4) && ((d8 & 1) == 0)))
    xnor = 1;

  uint16_t d_out = d8 & 1;
  uint16_t qi = d_out;

  for (int i = 1; i < 8; i++)
  {
    d_out |= ((qi << 1) ^ (d8 & (1u << i))) ^ (xnor << i);
    qi = d_out & (1u << i);
  }

  if (xnor == 1)
    d_out |= 1u << 9;
  else
    d_out |= 1u << 8;

  return d_out;
}

static uint16_t __not_in_flash_func(render_i2c_osd_line)(uint16_t y, const struct display *display, uint64_t *line_buf, uint8_t *scr_buf)
{
    uint16_t pixels = 0;

    // Оригинальный OSD Готека (верх экрана)
    if (display->on) {
        if ((display->rows > 0) && (y < (display->rows * FONT_HEIGHT))) {
            const uint8_t *t = display->text[y / FONT_HEIGHT];
            bool line_empty = true;
            for (unsigned int x = 0; x < display->cols; x++)
                line_empty &= (t[x] <= 0x20);
            if (line_empty)
                return pixels;

            // Оригинальный рендеринг OSD Готека с прозрачным фоном
            uint8_t border_color = 0;
            if (screen_buf) {
                border_color = screen_buf[(4 * V_BUF_W + 4) / 2] & 0x0F;
            }
            uint8_t text_color;
            if (border_color & 0x08) {
                text_color = 0;
            } else {
                text_color = ((border_color & 0x07) > 3) ? 0 : 15;
            }
            uint64_t bg_color = palette[border_color * 2];
            uint64_t fg_color = palette[text_color * 2];

            for (unsigned int x = 0; x < display->cols; x++) {
                uint8_t c = *t++;
                if ((c < 0x20) || (c > 0xf1)) c = 0x20;
                c -= 0x20;
                uint8_t glyph_line = font[(c * FONT_HEIGHT) + (y % FONT_HEIGHT)];
                for (int8_t bit = 7; bit >= 0; bit--) {
                    if ((glyph_line >> bit) & 1) {
                        *line_buf++ = fg_color;
                        *line_buf++ = fg_color ^ 0x0003ffffffffffffl;
                    } else {
                        *line_buf++ = bg_color;
                        *line_buf++ = bg_color ^ 0x0003ffffffffffffl;
                    }
                    pixels++;
                }
            }
            // Не выходим: даём возможности далее отрисовать меню (если попадаем в его вертикальный диапазон)
        }
    }

    // Рендеринг OSD меню (центр экрана)
    bool menu_active_now = osd_menu_is_active();
    if(menu_active_now)
    {
        const uint8_t rows_count = osd_menu_rows_count();
        if (rows_count > 0)
        {
            // Фиксированная ширина меню в символах
            const uint8_t max_cols = 25;
            const uint16_t menu_width = (uint16_t)max_cols * 8; // символов * 8 пикселей
            const uint16_t menu_height = rows_count * FONT_HEIGHT;
            const uint16_t pad_x = 0;
            const uint16_t pad_y = 0;

            // Координаты меню
            const uint16_t bg_width = menu_width + (pad_x << 1);
            const uint16_t bg_height = menu_height + (pad_y << 1);
            const uint16_t hv_pixels = (uint16_t)(video_mode.h_visible_area / 2);
            uint16_t bg_start_x = (hv_pixels > bg_width) ? (uint16_t)((hv_pixels - bg_width) / 2) : 0;
            bg_start_x &= (uint16_t)~1;
            const uint16_t v_visible_scaled = (uint16_t)(video_mode.v_visible_area / video_mode.div);
            const uint16_t menu_start_y = (v_visible_scaled > bg_height) ? (uint16_t)((v_visible_scaled - bg_height) / 2) : 0;

            if(y >= menu_start_y && y < (uint16_t)(menu_start_y + bg_height))
            {
                const uint16_t y_in_bg = y - menu_start_y;
                const bool in_text_band = (y_in_bg >= pad_y) && (y_in_bg < (uint16_t)(pad_y + menu_height));
                const uint16_t content_line = in_text_band ? (uint16_t)(y_in_bg - pad_y) : 0;
                const uint16_t menu_row = in_text_band ? (content_line / FONT_HEIGHT) : 0;

                // Цвета меню: серый (тускло-белый) фон (7), чёрный текст (0)
                const uint64_t menu_bg_color = palette[7 * 2];  // фон
                const uint64_t menu_fg_color = palette[0 * 2];  // текст

                // Левый участок до фона: копируем исходное видео из scr_buf
                const uint16_t left_pairs = bg_start_x >> 1;
                for (uint16_t w = 0; w < left_pairs; w++) {
                    uint8_t c2 = *scr_buf++;
                    uint64_t *c64 = &palette[(c2 & 0x0f) * 2];
                    *line_buf++ = *c64++;
                    *line_buf++ = *c64;
                    c2 >>= 4;
                    c64 = &palette[(c2 & 0x0f) * 2];
                    *line_buf++ = *c64++;
                    *line_buf++ = *c64;
                    pixels += 2;
                }

                const uint16_t bg_end_x = (uint16_t)(bg_start_x + bg_width);

                // Предвычисляем инверсию для записи пар TMDS-слов
                const uint64_t menu_bg_color2 = menu_bg_color ^ 0x0003ffffffffffffl;
                const uint64_t menu_fg_color2 = menu_fg_color ^ 0x0003ffffffffffffl;

                // Получаем строку меню один раз на линию
                const uint8_t *t_row = in_text_band ? osd_menu_get_row_ptr((uint8_t)menu_row) : NULL;
                const uint8_t glyph_line_mod = (uint8_t)(content_line % FONT_HEIGHT);

                // Рисуем внутреннюю область: либо сплошной фон, либо символы по 8 пикселей
                if (!in_text_band || !t_row) {
                    // Только фон
                    for (uint16_t x = 0; x < bg_width; x++) {
                        *line_buf++ = menu_bg_color;
                        *line_buf++ = menu_bg_color2;
                    }
                    pixels += bg_width;
                } else {
                    // Текст: 25 символов по 8 пикселей
                    for (uint16_t ch_idx = 0; ch_idx < max_cols; ch_idx++) {
                        uint8_t c = t_row[ch_idx];
                        if ((c < 0x20) || (c > 0xf1)) c = 0x20;
                        c -= 0x20;
                        const uint8_t glyph_line = font[(c * FONT_HEIGHT) + glyph_line_mod];
                        for (int8_t bit = 7; bit >= 0; bit--) {
                            const uint8_t mono = (glyph_line >> bit) & 1;
                            const uint64_t px = mono ? menu_fg_color : menu_bg_color;
                            *line_buf++ = px;
                            *line_buf++ = px ^ 0x0003ffffffffffffl;
                        }
                    }
                    pixels += menu_width;
                }
                return pixels;
            }
        }
    }

    return pixels;
}

static void __not_in_flash_func(dma_handler_dvi)()
{
  static uint16_t dma_buf_idx;
  static uint16_t y;

  dma_hw->ints0 = 1u << dma_ch1;

  dma_channel_set_read_addr(dma_ch1, &v_out_dma_buf_addr[dma_buf_idx & 1], false);

  y++;

  if (y == video_mode.whole_frame)
  {
    y = 0;
    screen_buf = (uint8_t*)get_v_buf_out();
  }

  if (y & 1)
    return;

  dma_buf_idx++;

  uint64_t *active_buf = (uint64_t *)(v_out_dma_buf[dma_buf_idx & 1]);
  bool have_video = (screen_buf != NULL);

  if (y < video_mode.v_visible_area)
  {
    // image area
    static uint8_t zero_line[V_BUF_W / 2] = {0};
    uint8_t *scr_buf = have_video ? &screen_buf[(uint16_t)(y / video_mode.div) * V_BUF_W / 2] : zero_line;
    uint64_t *line_buf = active_buf;

    // Рендерим OSD Готека (верх экрана) и меню (центр)
    uint16_t pixels = render_i2c_osd_line((y / video_mode.div), &i2c_display, active_buf, scr_buf);
    line_buf += (pixels << 1);
    scr_buf += (pixels >> 1);

    if (have_video) {
      for (int i = h_visible_area - (pixels >> 1); i--;)
      {
        uint8_t c2 = *scr_buf++;
        uint64_t *c64 = &palette[(c2 & 0xf) * 2];
        *line_buf++ = *c64++;
        *line_buf++ = *c64;
        c2 >>= 4;
        c64 = &palette[(c2 & 0xf) * 2];
        *line_buf++ = *c64++;
        *line_buf++ = *c64;
      }
    } else {
      // заполнение остатка строки чёрным, если захвата нет
      for (int i = h_visible_area - (pixels >> 1); i--;)
      {
        *line_buf++ = palette[0];
        *line_buf++ = palette[1];
        *line_buf++ = palette[0];
        *line_buf++ = palette[1];
      }
    }

    // horizontal sync
    memset64(active_buf + video_mode.h_visible_area, sync_data[0b00], video_mode.h_front_porch);
    memset64(active_buf + video_mode.h_visible_area + video_mode.h_front_porch, sync_data[0b01], video_mode.h_sync_pulse);
    memset64(active_buf + video_mode.h_visible_area + video_mode.h_front_porch + video_mode.h_sync_pulse, sync_data[0b00], video_mode.h_back_porch);
  }
  else if (y >= (video_mode.v_visible_area + video_mode.v_front_porch) && y < (video_mode.v_visible_area + video_mode.v_front_porch + video_mode.v_sync_pulse))
  {
    // vertical sync pulse
    memset64(active_buf, sync_data[0b10], video_mode.h_visible_area + video_mode.h_front_porch);
    memset64(active_buf + video_mode.h_visible_area + video_mode.h_front_porch, sync_data[0b11], video_mode.h_sync_pulse);
    memset64(active_buf + video_mode.h_visible_area + video_mode.h_front_porch + video_mode.h_sync_pulse, sync_data[0b10], video_mode.h_back_porch);
  }
  else
  {
    // vertical sync back porch
    memset64(active_buf, sync_data[0b00], video_mode.h_visible_area + video_mode.h_front_porch);
    memset64(active_buf + video_mode.h_visible_area + video_mode.h_front_porch, sync_data[0b01], video_mode.h_sync_pulse);
    memset64(active_buf + video_mode.h_visible_area + video_mode.h_front_porch + video_mode.h_sync_pulse, sync_data[0b00], video_mode.h_back_porch);
  }
}

void start_dvi(video_mode_t v_mode)
{
  video_mode = v_mode;
  // Индикация: HDMI старт — синий
  set_led(true, LED_BLUE);

  h_visible_area = video_mode.h_visible_area / (2 * video_mode.div);

  // initialization of constants
  uint16_t b0 = 0b1101010100;
  uint16_t b1 = 0b0010101011;
  uint16_t b2 = 0b0101010100;
  uint16_t b3 = 0b1010101011;

  sync_data[0b00] = get_ser_diff_data(b0, b0, b3);
  sync_data[0b01] = get_ser_diff_data(b0, b0, b2);
  sync_data[0b10] = get_ser_diff_data(b0, b0, b1);
  sync_data[0b11] = get_ser_diff_data(b0, b0, b0);

  R64 = get_ser_diff_data(tmds_encoder(255), tmds_encoder(0), tmds_encoder(0));
  G64 = get_ser_diff_data(tmds_encoder(0), tmds_encoder(255), tmds_encoder(0));
  B64 = get_ser_diff_data(tmds_encoder(0), tmds_encoder(0), tmds_encoder(255));
  Y64 = get_ser_diff_data(tmds_encoder(255), tmds_encoder(255), tmds_encoder(0));

  vreg_set_voltage(VREG_VOLTAGE_1_25);
  sleep_ms(100);
  set_sys_clock_khz(video_mode.sys_freq, true);
  sleep_ms(10);

  // palette initialization
  for (int c = 0; c < 16; c++)
  {
    uint8_t Y = (c >> 3) & 1;
    uint8_t R = ((c >> 2) & 1) ? (Y ? 255 : 170) : 0;
    uint8_t G = ((c >> 1) & 1) ? (Y ? 255 : 170) : 0;
    uint8_t B = ((c >> 0) & 1) ? (Y ? 255 : 170) : 0;
    palette[c * 2] = get_ser_diff_data(tmds_encoder(R), tmds_encoder(G), tmds_encoder(B));
    palette[c * 2 + 1] = palette[c * 2] ^ 0x0003ffffffffffffl;
  }

  // Установим начальный буфер вывода
  screen_buf = (uint8_t*)get_v_buf_out();

  if (!v_out_dma_buf[0]) v_out_dma_buf[0] = calloc(video_mode.whole_line * 2, sizeof(uint32_t));
  if (!v_out_dma_buf[1]) v_out_dma_buf[1] = calloc(video_mode.whole_line * 2, sizeof(uint32_t));

  // set DVI data pins
  for (int i = DVI_PIN_D0; i < DVI_PIN_D0 + 6; i++)
  {
    gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
    pio_gpio_init(PIO_DVI, i);
    gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
  }

  // set DVI clock pins
  for (int i = DVI_PIN_CLK0; i < DVI_PIN_CLK0 + 2; i++)
  {
    pio_gpio_init(PIO_DVI, i);
    gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
  }

  // PIO initialization
  // PIO program load
  uint offset = pio_add_program(PIO_DVI, &pio_program_dvi);

  pio_sm_config c = pio_get_default_sm_config();

  pio_sm_set_pins_with_mask(PIO_DVI, SM_DVI, 3u << DVI_PIN_CLK0, 3u << DVI_PIN_CLK0);
  pio_sm_set_pindirs_with_mask(PIO_DVI, SM_DVI, 3u << DVI_PIN_CLK0, 3u << DVI_PIN_CLK0);
  pio_sm_set_consecutive_pindirs(PIO_DVI, SM_DVI, DVI_PIN_D0, 6, true);

  sm_config_set_wrap(&c, offset, offset + (pio_program_dvi.length - 1));
  sm_config_set_out_shift(&c, true, true, 30);
  sm_config_set_out_pins(&c, DVI_PIN_D0, 6);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

  // PIO side set pins
  sm_config_set_sideset_pins(&c, DVI_PIN_CLK0);
  sm_config_set_sideset(&c, 2, false, false);

  pio_sm_init(PIO_DVI, SM_DVI, offset, &c);
  // Жёсткий сброс SM и FIFO перед стартом DMA
  pio_sm_set_enabled(PIO_DVI, SM_DVI, false);
  pio_sm_clear_fifos(PIO_DVI, SM_DVI);
  pio_sm_restart(PIO_DVI, SM_DVI);
  while (!pio_sm_is_tx_fifo_empty(PIO_DVI, SM_DVI)) {}

  // DMA initialization
  static int dma_ch0 = -1;
  if (dma_ch0 == -1) dma_ch0 = dma_claim_unused_channel(true);
  dma_ch1 = dma_claim_unused_channel(true);

  // main (data) DMA channel
  dma_channel_config c0 = dma_channel_get_default_config(dma_ch0);

  channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);
  channel_config_set_read_increment(&c0, true);
  channel_config_set_write_increment(&c0, false);
  channel_config_set_dreq(&c0, DREQ_PIO_DVI + SM_DVI);
  channel_config_set_chain_to(&c0, dma_ch1); // chain to control channel

  dma_channel_configure(
      dma_ch0,
      &c0,
      &PIO_DVI->txf[SM_DVI],     // write address
      &v_out_dma_buf[0][0],      // read address (одна или две строки)
      video_mode.whole_line * 2, //
      false                      // don't start yet
  );

  // control DMA channel
  dma_channel_config c1 = dma_channel_get_default_config(dma_ch1);

  channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);
  channel_config_set_read_increment(&c1, false);
  channel_config_set_write_increment(&c1, false);
  channel_config_set_chain_to(&c1, dma_ch0); // chain to other channel

  v_out_dma_buf_addr[0] = &v_out_dma_buf[0][0];
  v_out_dma_buf_addr[1] = &v_out_dma_buf[1][0];

  dma_channel_configure(
      dma_ch1,
      &c1,
      &dma_hw->ch[dma_ch0].read_addr, // write address
      &v_out_dma_buf_addr[0],         // read address
      1,                              //
      false                           // don't start yet
  );

  dma_channel_set_irq0_enabled(dma_ch1, true);

  // configure the processor to run dma_handler() when DMA IRQ 0 is asserted
  irq_set_exclusive_handler(DMA_IRQ_0, dma_handler_dvi);

  // Умеренный приоритет, чтобы не блокировать захват можно попробовать 0x00 если тормозит
  irq_set_priority(DMA_IRQ_0, 0x40);
  irq_set_enabled(DMA_IRQ_0, true);

  // Предзаполняем обе строки «чёрной» картинкой и корректными HSYNC сегментами,
  for (int buf_idx = 0; buf_idx < 2; ++buf_idx) {
    uint64_t *active_buf = (uint64_t *)(v_out_dma_buf[buf_idx]);
    uint64_t *line_buf = active_buf;
    for (int i = h_visible_area; i--; ) {
      *line_buf++ = palette[0];
      *line_buf++ = palette[1];
      *line_buf++ = palette[0];
      *line_buf++ = palette[1];
    }
    memset64(active_buf + video_mode.h_visible_area, sync_data[0b00], video_mode.h_front_porch);
    memset64(active_buf + video_mode.h_visible_area + video_mode.h_front_porch, sync_data[0b01], video_mode.h_sync_pulse);
    memset64(active_buf + video_mode.h_visible_area + video_mode.h_front_porch + video_mode.h_sync_pulse, sync_data[0b00], video_mode.h_back_porch);
  }

  dma_start_channel_mask((1u << dma_ch0));

  // После старта DMA включаем PIO SM
  pio_sm_set_enabled(PIO_DVI, SM_DVI, true);
}
