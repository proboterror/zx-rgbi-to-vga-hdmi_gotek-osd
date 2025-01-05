/*
  GOTEK floppy drive emulator with flashfloppy firmware I2C LCD OSD interface. 

  VGA and HDMI OSD output implemented.

  Based on https://github.com/keirf/flashfloppy-osd/ by Keir Fraser
  Implementation: https://github.com/proboterror

  I2C communications to the host:
  1. Emulate HD44780 LCD controller via a PCF8574 I2C backpack.
  Supported screen size 20x4 characters.
  2. Support extended custom FF OSD protocol with bidirectional comms.

  Simultaneous OLED/LCD screen and on-screen display can be supported with FF OSD protocol build enabled.

  GOTEK configuration:
  https://github.com/keirf/flashfloppy/wiki/Hardware-Mods#lcd-display
  https://github.com/keirf/flashfloppy/wiki/FF.CFG-Configuration-File

  FF.CFG:

  for FF OSD protocol with dual OLED/LCD support:
  set:
  display-type=auto
  or
  display-type = oled-128x64
  with
  osd-display-order = 3,0
  osd-columns = 36
  and 
  display-off-secs = 0-255 (60 by default)

  display-order and osd-display-order can be set independently.

  for single PCF8574 20x4 LCD display protocol:
  set:
  display-type=lcd-20x04
  with
  display-order=3,0,2,1
  and
  display-off-secs = 0-255 (60 by default)

  Wiring:
  I2C uses 2 wires: SDA and SCL to connect RGBI2VGA adapter and GOTEK.
  Connect GOTEK SDA and SCL pins to free Pico I2C pins pair: see I2C_SLAVE_SDA_PIN, I2C_SLAVE_SCL_PIN constants declaration.
  SDA and SCL lines should be pulled up to VCC(3.3V) with 4.7~10K resistors on GOTEK or RGBI2VGA side.

  WaveShare RP2040-Zero board not supported.

  Russian filenames are supported, requires https://github.com/proboterror/flashfloppy-russian patched flashfloppy GOTEK firmware. 
*/

#include "g_config.h"
#include "gotek_i2c_osd.h"
#include "i2c_slave.h" // ToDo: import pico_i2c_slave library from pico-sdk with CMake (pico-sdk/src/rp2_common/pico_i2c_slave) 

#include <memory.h>

#include <hardware/i2c.h>
#include <pico/stdlib.h>

#define FF_OSD_SUPPORT 1

#if FF_OSD_SUPPORT
static const uint I2C_SLAVE_ADDRESS = 0x10; // Fixed address custom FF OSD protocol
#else
static const uint I2C_SLAVE_ADDRESS = 0x27; // Most common address for PCF8574 displays
#endif
static const uint I2C_BAUDRATE = 100000; // 100 kHz: I2C Standard Mode, matching flashfloppy setting 

// Use GP16/17(I2C0), GP18/19 (I2C1), GP20/21 (I2C0), GP26/27 (I2C1) with full size Raspberry Pi Pico board.
// Note: I2C0 passed to i2c_slave_init by default.
static const uint I2C_SLAVE_SDA_PIN = 16; 
static const uint I2C_SLAVE_SCL_PIN = 17;

/* I2C data ring. */
static uint8_t d_ring[1024];
static uint16_t d_cons, d_prod; // data ring buffer consumer / producer pointers 

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define MASK(r,x) ((x) & (ARRAY_SIZE(r)-1))

/* Transaction ring: Data-ring offset of each transaction start. */
static uint16_t t_ring[8]; 
static uint16_t t_cons, t_prod; // transactions ring buffer consumer / producer pointers

/* Display state, exported to display routines. */
struct display i2c_display = { .rows = 4, .cols = 20, .on = false, .text = {} };

/* LCD state. */
static bool lcd_inc;
static uint8_t lcd_ddraddr;

#if FF_OSD_SUPPORT

/* I2C custom protocol state. */
struct i2c_osd_info
{
    uint8_t protocol_ver;
    uint8_t fw_major, fw_minor;
    uint8_t buttons;
} i2c_osd_info = {.protocol_ver = 0, .fw_major = '1', .fw_minor = '9'}; /* state: OSD -> Gotek */

/* Current position in FF OSD I2C Protocol character data. */
static uint8_t ff_osd_x, ff_osd_y;

/* FF OSD command set */
#define OSD_BACKLIGHT    0x00 /* [0] = backlight on */
#define OSD_DATA         0x02 /* next columns*rows bytes are text data */
#define OSD_ROWS         0x10 /* [3:0] = #rows */
#define OSD_HEIGHTS      0x20 /* [3:0] = 1 iff row is 2x height */
#define OSD_BUTTONS      0x30 /* [3:0] = button mask */
#define OSD_COLUMNS      0x40 /* [6:0] = #columns */

#endif // FF_OSD_SUPPORT

/* PCF8574 pin assignment: D7-D6-D5-D4-BL-EN-RW-RS */
#define _D7 (1u<<7)
#define _D6 (1u<<6)
#define _D5 (1u<<5)
#define _D4 (1u<<4)
#define _BL (1u<<3)
#define _EN (1u<<2)
#define _RW (1u<<1)
#define _RS (1u<<0)

#define min_t(type,x,y) \
    ({ type __x = (x); type __y = (y); __x < __y ? __x: __y; })

struct config_t
{
    uint16_t min_cols, max_cols;
    uint16_t rows;
} config_t;

const static struct config_t config =
{
    .min_cols = 16,
    .max_cols = 40,
    .rows = 4,
};

#if FF_OSD_SUPPORT
#define barrier() asm volatile ("" ::: "memory")

static void ff_osd_process(void)
{
    uint16_t d_c, d_p, t_c, t_p;

    d_c = d_cons;
    d_p = d_prod;
    barrier(); /* Get data ring producer /then/ transaction ring producer */
    t_c = t_cons;
    t_p = t_prod;

    /* We only care about the last full transaction, and newer. */
    if ((uint16_t)(t_p - t_c) >= 2) {
        /* Discard older transactions, and in-progress old transaction. */
        t_c = t_p - 2;
        d_c = t_ring[MASK(t_ring, t_c)];
        ff_osd_y = 0;
    }

    /* Data ring should not be more than half full. We don't want it to 
     * overrun during the processing loop below: That should be impossible
     * with half a ring free. */
    //assert((uint16_t)(d_p - d_c) < (ARRAY_SIZE(d_ring)/2));

    /* Process the command sequence. */
    for (; d_c != d_p; d_c++) {
        uint8_t x = d_ring[MASK(d_ring, d_c)];
        if ((t_c != t_p) && (d_c == t_ring[MASK(t_ring, t_c)])) {
            t_c++;
            ff_osd_y = 0;
        }
        if (ff_osd_y != 0) {
            /* Character Data. */
            i2c_display.text[ff_osd_y-1][ff_osd_x] = x;
            if (++ff_osd_x >= i2c_display.cols) {
                ff_osd_x = 0;
                if (++ff_osd_y > i2c_display.rows)
                    ff_osd_y = 0;
            }
        } else {
            /* Command. */
            if ((x & 0xc0) == OSD_COLUMNS) {
                /* 0-40 */
                i2c_display.cols = min_t(uint16_t, 40, x & 0x3f);
            } else {
                switch (x & 0xf0) {
                case OSD_BUTTONS:
                    /* button state: Gotek -> OSD */
                    uint8_t i2c_buttons_rx = x & 0x0f;
                    break;
                case OSD_ROWS:
                    /* Limit max rows count to 4 */
                    i2c_display.rows = x > 4 ? 4 : x;
                    break;
                case OSD_HEIGHTS:
                    /* FF OSD custom text lines heights. */
                    /* One bit for each text row: 0: single height, 1: double height. */ 
                    uint8_t heights = x & 0x0f;
                    break;
                case OSD_BACKLIGHT:
                    switch (x & 0x0f) {
                    case 0:
                        i2c_display.on = false;
                        break;
                    case 1:
                        i2c_display.on = true;
                        break;
                    case 2:
                        ff_osd_x = 0;
                        ff_osd_y = 1;
                        break;
                    }
                }
            }
        }
    }

    d_cons = d_c;
    t_cons = t_c;
}
#endif // FF_OSD_SUPPORT

static void lcd_process_cmd(uint8_t cmd)
{
    uint8_t x = 0x80;
    int c = 0;

    if (!cmd)
        return;

    while (!(cmd & x)) {
        x >>= 1;
        c++;
    }

    switch (c) {
    case 0: /* Set DDR Address */
        lcd_ddraddr = cmd & 127;
        break;
    case 1: /* Set CGR Address */
        break;
    case 2: /* Function Set */
        break;
    case 3: /* Cursor or Display Shift */
        break;
    case 4: /* Display On/Off Control */
        break;
    case 5: /* Entry Mode Set */
        lcd_inc = !!(cmd & 2);
        break;
    case 6: /* Return Home */
        lcd_ddraddr = 0;
        break;
    case 7: /* Clear Display */
        memset(i2c_display.text, ' ', sizeof(i2c_display.text));
        lcd_ddraddr = 0;
        break;
    }
}

static void lcd_process_dat(uint8_t dat)
{
    int x, y;
    if (lcd_ddraddr >= 0x68)
        lcd_ddraddr = 0x00; /* jump to line 2 */
    if ((lcd_ddraddr >= 0x28) && (lcd_ddraddr < 0x40))
        lcd_ddraddr = 0x40; /* jump to line 1 */
    x = lcd_ddraddr & 0x3f;
    y = lcd_ddraddr >> 6;
    if ((i2c_display.rows == 4) && (x >= 20)) {
        x -= 20;
        y += 2;
    }
    i2c_display.text[y][x] = dat;
    lcd_ddraddr++;
    if (x >= i2c_display.cols)
        i2c_display.cols = min_t(unsigned int, x+1, config.max_cols);
}

static void lcd_process(void)
{
    uint16_t d_c, d_p = d_prod;
    static uint16_t dat = 1;
    static bool rs;

    /* Process the command sequence. */
    for (d_c = d_cons; d_c != d_p; d_c++) {
        uint8_t x = d_ring[MASK(d_ring, d_c)];
        if ((x & (_EN|_RW)) != _EN)
            continue;
        i2c_display.on = !!(x & _BL);
        if (rs != !!(x & _RS)) {
            rs ^= 1;
            dat = 1;
        }
        dat <<= 4;
        dat |= x >> 4;
        if (dat & 0x100) {
            if (rs)
                lcd_process_dat(dat);
            else
                lcd_process_cmd(dat);
            dat = 1;
        }
    }

    d_cons = d_c;
}

void i2c_slave_handler(i2c_inst_t *i2c, i2c_slave_event_t event)
{
    static uint8_t rp;

    switch (event)
    {
    case I2C_SLAVE_RECEIVE: // master has written some data
        d_ring[MASK(d_ring, d_prod++)] = i2c_read_byte_raw(i2c);
        break;
    case I2C_SLAVE_REQUEST: // master is requesting data
#if FF_OSD_SUPPORT    
        uint8_t *info = (uint8_t *)&i2c_osd_info;
        i2c_write_byte_raw(i2c, (rp < sizeof(i2c_osd_info)) ? info[rp++] : 0);
#endif
        break;
    case I2C_SLAVE_FINISH: // master has signalled Stop / Restart
        t_ring[MASK(t_ring, t_prod++)] = d_prod;
        rp = 0;
        break;
    default:
        break;
    }
}

void osd_process()
{
#if FF_OSD_SUPPORT
    ff_osd_process();
#else
    lcd_process();
#endif
}

void setup_i2c_slave()
{
    gpio_init(I2C_SLAVE_SDA_PIN);
    gpio_set_function(I2C_SLAVE_SDA_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SLAVE_SDA_PIN);

    gpio_init(I2C_SLAVE_SCL_PIN);
    gpio_set_function(I2C_SLAVE_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SLAVE_SCL_PIN);

    i2c_init(i2c0, I2C_BAUDRATE);
    // configure I2C0 for slave mode
    i2c_slave_init(i2c0, I2C_SLAVE_ADDRESS, &i2c_slave_handler);
}