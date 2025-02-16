#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"

#include "g_config.h"
#include "settings.h"

extern volatile bool stop_core1;
extern volatile bool core1_inactive;

extern settings_t settings;

// https://en.wikipedia.org/wiki/Fletcher's_checksum
uint16_t Fletcher16(uint8_t *data, int count)
{
  uint16_t sum1 = 0;
  uint16_t sum2 = 0;
  int index;

  for (index = 0; index < count; ++index)
  {
    sum1 = (sum1 + data[index]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }

  return (sum2 << 8) | sum1;
}

typedef struct flash_settings_t
{
  settings_t settings;
  uint16_t checksum;
} flash_settings_t;

void check_settings(settings_t *settings)
{
  if (settings->video_out_type > VIDEO_OUT_TYPE_MAX ||
      settings->video_out_type < VIDEO_OUT_TYPE_MIN)
    settings->video_out_type = VIDEO_OUT_TYPE_DEF;

  if (settings->video_out_mode > VIDEO_OUT_MODE_MAX ||
      (settings->video_out_type == DVI && settings->video_out_mode > VIDEO_MODE_DVI_MAX) || // DVI mode supports resolutions up to VIDEO_MODE_DVI_MAX
      settings->video_out_mode < VIDEO_OUT_MODE_MIN)
    settings->video_out_mode = VIDEO_OUT_MODE_DEF;

  if (settings->cap_sync_mode > CAP_SYNC_MODE_MAX ||
      settings->cap_sync_mode < CAP_SYNC_MODE_MIN)
    settings->cap_sync_mode = CAP_SYNC_MODE_DEF;

  if (settings->frequency > FREQUENCY_MAX ||
      settings->frequency < FREQUENCY_MIN)
    settings->frequency = FREQUENCY_DEF;

  if (settings->ext_clk_divider > EXT_CLK_DIVIDER_MAX ||
      settings->ext_clk_divider < EXT_CLK_DIVIDER_MIN)
    settings->ext_clk_divider = EXT_CLK_DIVIDER_DEF;

  if (settings->delay > DELAY_MAX ||
      settings->delay < DELAY_MIN)
    settings->delay = DELAY_DEF;

  if (settings->shX > shX_MAX ||
      settings->shX < shX_MIN)
    settings->shX = shX_DEF;

  if (settings->shY > shY_MAX ||
      settings->shY < shY_MIN)
    settings->shY = shY_DEF;

  if (settings->pin_inversion_mask & ~PIN_INVERSION_MASK)
  {
    settings->pin_inversion_mask = PIN_INVERSION_MASK_DEF;
    settings->scanlines_mode = false;
    settings->buffering_mode = false;
    settings->video_sync_mode = false;
  }
}

void load_settings(settings_t *settings)
{
  const int *saved_settings = (const int *)(XIP_BASE + (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE));

  const flash_settings_t *flash_settings = (flash_settings_t*)saved_settings;
  const uint16_t checksum = Fletcher16((uint8_t*)saved_settings, sizeof(settings_t));

  if(checksum == flash_settings->checksum)
    memcpy(settings, saved_settings, sizeof(settings_t));

  check_settings(settings);
}

void save_settings(settings_t *settings)
{
  check_settings(settings);

  flash_settings_t* flash_settings = (flash_settings_t*)malloc(FLASH_PAGE_SIZE);

  flash_settings->settings = *settings;
  flash_settings->checksum = Fletcher16((uint8_t*)settings, sizeof(settings_t));

  stop_core1 = true;

  while (!core1_inactive)
    ;

  uint32_t ints = save_and_disable_interrupts();

  flash_range_erase((PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE), FLASH_SECTOR_SIZE);
  flash_range_program((PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE), (uint8_t *)flash_settings, FLASH_PAGE_SIZE);

  restore_interrupts_from_disabled(ints);

  free(flash_settings);

  stop_core1 = false;
  core1_inactive = false;
}