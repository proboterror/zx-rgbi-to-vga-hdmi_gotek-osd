#include "ps2_keyboard.h"
#include "CH446Q.h"
#include "gotek_i2c_osd.h"

#include <inttypes.h>

/*
ZX Spectrum 40-key keyboard matrix:
         ┌─────────────────────────────────────────────────────┐
         │     ┌─────────────────────────────────────────┐     │
         │     │     ┌─────────────────────────────┐     │     │
         │     │     │     ┌─────────────────┐     │     │     │
         │     │     │     │     ┌─────┐     │     │     │     │
       ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐
 A11/ ─┤ 1 ├─┤ 2 ├─┤ 3 ├─┤ 4 ├─┤ 5 │ │ 6 ├─┤ 7 ├─┤ 8 ├─┤ 9 ├─┤ 0 ├─ A12/
       └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘
       ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐
 A10/ ─┤ Q ├─┤ W ├─┤ E ├─┤ R ├─┤ T │ │ Y ├─┤ U ├─┤ I ├─┤ O ├─┤ P ├─ A13/
       └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘
       ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐
  A9/ ─┤ A ├─┤ S ├─┤ D ├─┤ F ├─┤ G │ │ H ├─┤ J ├─┤ K ├─┤ L ├─┤Ent├─ A14/
       └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘
       ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐ ┌─┴─┐
  A8/ ─┤ CS├─┤ Z ├─┤ X ├─┤ C ├─┤ V │ │ B ├─┤ N ├─┤ M ├─┤ SS├─┤ SP├─ A15/
       └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘
         D0    D1    D2    D3    D4    D4    D3    D2    D1    D0

ZX Spectrum+ extended 58 keys keyboard:
 CS + 1 "EDIT"
 CS + 2 "CAPS LOCK"
 CS + 3 "TRUE VIDEO"
 CS + 4 "INV VIDEO"
 CS + 5 "LEFT"
 CS + 6 "DOWN"
 CS + 7 "UP"
 CS + 8 "RIGHT"
 CS + 9 "GRAPH"
 CS + 0 "DELETE"

 CS + SP "BREAK"
 CS + SS "EXTENDED MODE"

 SS + N ","
 SS + M "."
 SS + O ";"
 SS + P """
*/

/*
8x16 Analog Switch Array Chip CH446Q

The following table is the decoding truth table of the 7-bit address ADDR for CH446Q chip and the address table of 128 analog switches.

Intersection
Point          ADDR6 ADDR5 ADDR4 ADDR3 ADDR2 ADDR1 ADDR0 Address
Port Y -        AY2   AY1   AY0   AX3   AX2   AX1   AX0    No.
Port X
Y0 - X0          0     0     0     0     0     0     0     00H
Y0 - X1          0     0     0     0     0     0     1     01H
Y0 - X2          0     0     0     0     0     1     0     02H
······
Y7 - X14         1     1     1     1     1     1     0     7EH
Y7 - X15         1     1     1     1     1     1     1     7FH

ZX Spectrum A8..A15 bus connected to CH446Q X0..X7 pins; D0..D4 bus connected to CH446Q Y0..Y4 pins.
*/
enum CH446Q_AY
{
	D0 = 0 << 4,
	D1 = 1 << 4,
	D2 = 2 << 4,
	D3 = 3 << 4,
	D4 = 4 << 4
};

enum CH446Q_AX
{
	A8  = 0,
	A9  = 1,
	A10 = 2,
	A11 = 3,
	A12 = 4,
	A13 = 5,
	A14 = 6,
	A15 = 7,
};

// ZX key code composed of CH446Q X and Y key matrix address
enum zx_keys
{
	ZX_KEY_CS  = D0 | A8,
	ZX_KEY_A   = D0 | A9,
	ZX_KEY_Q   = D0 | A10,
	ZX_KEY_1   = D0 | A11,
	ZX_KEY_0   = D0 | A12,
	ZX_KEY_P   = D0 | A13,
	ZX_KEY_ENT = D0 | A14,
	ZX_KEY_SP  = D0 | A15,
	ZX_KEY_Z   = D1 | A8,
	ZX_KEY_S   = D1 | A9,
	ZX_KEY_W   = D1 | A10,
	ZX_KEY_2   = D1 | A11,
	ZX_KEY_9   = D1 | A12,
	ZX_KEY_O   = D1 | A13,
	ZX_KEY_L   = D1 | A14,
	ZX_KEY_SS  = D1 | A15,
	ZX_KEY_X   = D2 | A8,
	ZX_KEY_D   = D2 | A9,
	ZX_KEY_E   = D2 | A10,
	ZX_KEY_3   = D2 | A11,
	ZX_KEY_8   = D2 | A12,
	ZX_KEY_I   = D2 | A13,
	ZX_KEY_K   = D2 | A14,
	ZX_KEY_M   = D2 | A15,
	ZX_KEY_C   = D3 | A8,
	ZX_KEY_F   = D3 | A9,
	ZX_KEY_R   = D3 | A10,
	ZX_KEY_4   = D3 | A11,
	ZX_KEY_7   = D3 | A12,
	ZX_KEY_U   = D3 | A13,
	ZX_KEY_J   = D3 | A14,
	ZX_KEY_N   = D3 | A15,
	ZX_KEY_V   = D4 | A8,
	ZX_KEY_G   = D4 | A9,
	ZX_KEY_T   = D4 | A10,
	ZX_KEY_5   = D4 | A11,
	ZX_KEY_6   = D4 | A12,
	ZX_KEY_Y   = D4 | A13,
	ZX_KEY_H   = D4 | A14,
	ZX_KEY_B   = D4 | A15,

	ZX_KEY_MAGIC = 0x58, // Y:5 X:8
	ZX_KEY_RESET = 0x68, // Y:6 X:8
	ZX_KEY_PAUSE = 0x78, // Y:7 X:8

	ZX_KEY_NONE= 0xFF
};

/*
 Reference:
 The PS/2 Keyboard Interface
 Source: http://www.Computer-Engineering.org/ps2keyboard/
 Author: Adam Chapweske
 Keyboard Scan Codes: Set 2

 Notes for PS/2 keyboard scan codes set 2 (default):

 - Following keys hame multi-byte scan codes:
  Key         Make Code   Break Code
  L GUI       E0,1F       E0,F0,1F
  R CTRL      E0,14       E0,F0,14
  R GUI       E0,27       E0,F0,27
  R ALT       E0,11       E0,F0,11
  APPS        E0,2F       E0,F0,2F
  PRNT SCRN   E0,12,      E0,F0,7C,
              E0,7C       E0,F0,12
  PAUSE       E1,14,77,
              E1,F0,14,   -NONE-
              F0,77
  INSERT      E0,70       E0,F0,70
  HOME        E0,6C       E0,F0,6C
  PG UP       E0,7D       E0,F0,7D
  DELETE      E0,71       E0,F0,71
  END         E0,69       E0,F0,69
  PG DN       E0,7A       E0,F0,7A
  U ARROW     E0,75       E0,F0,75
  L ARROW     E0,6B       E0,F0,6B
  D ARROW     E0,72       E0,F0,72
  R ARROW     E0,74       E0,F0,74
  KP /        E0,4A       E0,F0,4A
  KP EN       E0,5A       E0,F0,5A

 - Pause/Break key does not have a break code

 ACPI Scan Codes:
  Key   Make   Break
  Power E0,37  E0,F0,37
  Sleep E0,3F  E0,F0,3F
  Wake  E0,5E  E0,F0,5E

 Windows Multimedia Scan Codes:
  Key            Make   Break
  Next Track     E0,4D  E0,F0,4D
  Previous Track E0,15  E0,F0,15
  Stop           E0,3B  E0,F0,3B
  Play/Pause     E0,34  E0,F0,34
  Mute           E0,23  E0,F0,23
  Volume Up      E0,32  E0,F0,32
  Volume Down    E0,21  E0,F0,21
  Media Select   E0,50  E0,F0,50
  E-Mail         E0,48  E0,F0,48
  Calculator     E0,2B  E0,F0,2B
  My Computer    E0,40  E0,F0,40
  WWW Search     E0,10  E0,F0,10
  WWW Home       E0,3A  E0,F0,3A
  WWW Back       E0,38  E0,F0,38
  WWW Forward    E0,30  E0,F0,30
  WWW Stop       E0,28  E0,F0,28
  WWW Refresh    E0,20  E0,F0,20
  WWW Favorites  E0,18  E0,F0,18
*/

enum ps2_scan_codes_set_2
{
	//                     PS/2 SET 2, PS/2 SET 1 (XT/BIOS PC)
	PS2_KEY_ESCAPE       = 0x76 /*0x01*/,
	PS2_KEY_1            = 0x16 /*0x02*/,
	PS2_KEY_2            = 0x1E /*0x03*/,
	PS2_KEY_3            = 0x26 /*0x04*/,
	PS2_KEY_4            = 0x25 /*0x05*/,
	PS2_KEY_5            = 0x2E /*0x06*/,
	PS2_KEY_6            = 0x36 /*0x07*/,
	PS2_KEY_7            = 0x3D /*0x08*/,
	PS2_KEY_8            = 0x3E /*0x09*/,
	PS2_KEY_9            = 0x46 /*0x0A*/,
	PS2_KEY_0            = 0x45 /*0x0B*/,
	PS2_KEY_MINUS        = 0x4E /*0x0C*/, // - on main keyboard
	PS2_KEY_EQUALS       = 0x55 /*0x0D*/,
	PS2_KEY_BACK         = 0x66 /*0x0E*/, // backspace
	PS2_KEY_TAB          = 0x0D /*0x0F*/,
	PS2_KEY_Q            = 0x15 /*0x10*/,
	PS2_KEY_W            = 0x1D /*0x11*/,
	PS2_KEY_E            = 0x24 /*0x12*/,
	PS2_KEY_R            = 0x2D /*0x13*/,
	PS2_KEY_T            = 0x2C /*0x14*/,
	PS2_KEY_Y            = 0x35 /*0x15*/,
	PS2_KEY_U            = 0x3C /*0x16*/,
	PS2_KEY_I            = 0x43 /*0x17*/,
	PS2_KEY_O            = 0x44 /*0x18*/,
	PS2_KEY_P            = 0x4D /*0x19*/,
	PS2_KEY_LBRACKET     = 0x54 /*0x1A*/,
	PS2_KEY_RBRACKET     = 0x5B /*0x1B*/,
	PS2_KEY_RETURN       = 0x5A /*0x1C*/, // Enter on main keyboard
	PS2_KEY_LCONTROL     = 0x14 /*0x1D*/,
	PS2_KEY_A            = 0x1C /*0x1E*/,
	PS2_KEY_S            = 0x1B /*0x1F*/,
	PS2_KEY_D            = 0x23 /*0x20*/,
	PS2_KEY_F            = 0x2B /*0x21*/,
	PS2_KEY_G            = 0x34 /*0x22*/,
	PS2_KEY_H            = 0x33 /*0x23*/,
	PS2_KEY_J            = 0x3B /*0x24*/,
	PS2_KEY_K            = 0x42 /*0x25*/,
	PS2_KEY_L            = 0x4B /*0x26*/,
	PS2_KEY_SEMICOLON    = 0x4C /*0x27*/,
	PS2_KEY_APOSTROPHE   = 0x52 /*0x28*/,
	PS2_KEY_GRAVE        = 0x0E /*0x29*/, // accent grave
	PS2_KEY_LSHIFT       = 0x12 /*0x2A*/,
	PS2_KEY_BACKSLASH    = 0x5D /*0x2B*/,
	PS2_KEY_Z            = 0x1A /*0x2C*/,
	PS2_KEY_X            = 0x22 /*0x2D*/,
	PS2_KEY_C            = 0x21 /*0x2E*/,
	PS2_KEY_V            = 0x2A /*0x2F*/,
	PS2_KEY_B            = 0x32 /*0x30*/,
	PS2_KEY_N            = 0x31 /*0x31*/,
	PS2_KEY_M            = 0x3A /*0x32*/,
	PS2_KEY_COMMA        = 0x41 /*0x33*/,
	PS2_KEY_PERIOD       = 0x49 /*0x34*/, // . on main keyboard
	PS2_KEY_SLASH        = 0x4A /*0x35*/, // / on main keyboard
	PS2_KEY_RSHIFT       = 0x59 /*0x36*/,
	PS2_KEY_MULTIPLY     = 0x7C /*0x37*/, // * on numeric keypad
	PS2_KEY_LMENU        = 0x11 /*0x38*/, // left Alt
	PS2_KEY_SPACE        = 0x29 /*0x39*/,
	PS2_KEY_CAPITAL      = 0x58 /*0x3A*/,
	PS2_KEY_F1           = 0x05 /*0x3B*/,
	PS2_KEY_F2           = 0x06 /*0x3C*/,
	PS2_KEY_F3           = 0x04 /*0x3D*/,
	PS2_KEY_F4           = 0x0C /*0x3E*/,
	PS2_KEY_F5           = 0x03 /*0x3F*/,
	PS2_KEY_F6           = 0x0B /*0x40*/,
	PS2_KEY_F7           = 0x83 /*0x41*/,
	PS2_KEY_F8           = 0x0A /*0x42*/,
	PS2_KEY_F9           = 0x01 /*0x43*/,
	PS2_KEY_F10          = 0x09 /*0x44*/,
	PS2_KEY_NUMLOCK      = 0x77 /*0x45*/,
	PS2_KEY_SCROLL       = 0x7E /*0x46*/, // Scroll Lock
	PS2_KEY_NUMPAD7      = 0x6C /*0x47*/,
	PS2_KEY_NUMPAD8      = 0x75 /*0x48*/,
	PS2_KEY_NUMPAD9      = 0x7D /*0x49*/,
	PS2_KEY_SUBTRACT     = 0x7B /*0x4A*/, // - on numeric keypad
	PS2_KEY_NUMPAD4      = 0x6B /*0x4B*/,
	PS2_KEY_NUMPAD5      = 0x73 /*0x4C*/,
	PS2_KEY_NUMPAD6      = 0x74 /*0x4D*/,
	PS2_KEY_ADD          = 0x79 /*0x4E*/, // + on numeric keypad
	PS2_KEY_NUMPAD1      = 0x69 /*0x4F*/,
	PS2_KEY_NUMPAD2      = 0x72 /*0x50*/,
	PS2_KEY_NUMPAD3      = 0x7A /*0x51*/,
	PS2_KEY_NUMPAD0      = 0x70 /*0x52*/,
	PS2_KEY_DECIMAL      = 0x71 /*0x53*/, // . on numeric keypad
	PS2_KEY_F11          = 0x78 /*0x57*/,
	PS2_KEY_F12          = 0x07 /*0x58*/
#if 0
	PS2_KEY_NEXTTRACK    = 0xE0, 0x4D /*0x99*/, // Next Track
	PS2_KEY_NUMPADENTER  = 0xE0, 0x5A /*0x9C*/, // Enter on numeric keypad
	PS2_KEY_RCONTROL     = 0xE0, 0x14 /*0x9D*/,
	PS2_KEY_MUTE         = 0xE0, 0x23 /*0xA0*/, // Mute
	PS2_KEY_CALCULATOR   = 0xE0, 0x2B /*0xA1*/, // Calculator
	PS2_KEY_PLAYPAUSE    = 0xE0, 0x34 /*0xA2*/, // Play / Pause
	PS2_KEY_MEDIASTOP    = 0xE0, 0x3B /*0xA4*/, // Media Stop
	PS2_KEY_VOLUMEDOWN   = 0xE0, 0x21 /*0xAE*/, // Volume -
	PS2_KEY_VOLUMEUP     = 0xE0, 0x32 /*0xB0*/, // Volume +
	PS2_KEY_WEBHOME      = 0xE0, 0x3A /*0xB2*/, // Web home
	PS2_KEY_DIVIDE       = 0xE0, 0x4A /*0xB5*/, // / on numeric keypad
	PS2_KEY_SYSRQ        = 0xE0, 0x12, 0xE0, 0x7C /*0xB7*/,
	PS2_KEY_RMENU        = 0xE0, 0x11 /*0xB8*/, // Right Alt
	PS2_KEY_PAUSE        = 0xE1, 0x14, 77, E1, F0, 14, F0, 77 /*0xC5*/, // Pause
	PS2_KEY_HOME         = 0xE0, 0x6C /*0xC7*/, // Home on arrow keypad
	PS2_KEY_UP           = 0xE0, 0x75 /*0xC8*/, // UpArrow on arrow keypad
	PS2_KEY_PRIOR        = 0xE0, 0x7D /*0xC9*/, // PgUp on arrow keypad
	PS2_KEY_LEFT         = 0xE0, 0x6B /*0xCB*/, // LeftArrow on arrow keypad
	PS2_KEY_RIGHT        = 0xE0, 0x74 /*0xCD*/, // RightArrow on arrow keypad
	PS2_KEY_END          = 0xE0, 0x69 /*0xCF*/, // End on arrow keypad
	PS2_KEY_DOWN         = 0xE0, 0x72 /*0xD0*/, // DownArrow on arrow keypad
	PS2_KEY_NEXT         = 0xE0, 0x7A /*0xD1*/, // PgDn on arrow keypad
	PS2_KEY_INSERT       = 0xE0, 0x70 /*0xD2*/, // Insert on arrow keypad
	PS2_KEY_DELETE       = 0xE0, 0x71 /*0xD3*/, // Delete on arrow keypad
	PS2_KEY_LWIN         = 0xE0, 0x1F /*0xDB*/, // Left Windows key
	PS2_KEY_RWIN         = 0xE0, 0x27 /*0xDC*/, // Right Windows key
	PS2_KEY_APPS         = 0xE0, 0x2F /*0xDD*/, // AppMenu key
	PS2_KEY_POWER        = 0xE0, 0x37 /*0xDE*/, // System Power
	PS2_KEY_SLEEP        = 0xE0, 0x3F /*0xDF*/, // System Sleep
	PS2_KEY_WAKE         = 0xE0, 0x5E /*0xE3*/, // System Wake
	PS2_KEY_WEBSEARCH    = 0xE0, 0x10 /*0xE5*/, // Web Search
	PS2_KEY_WEBFAVORITES = 0xE0, 0x18 /*0xE6*/, // Web Favorites
	PS2_KEY_WEBREFRESH   = 0xE0, 0x20 /*0xE7*/, // Web Refresh
	PS2_KEY_WEBSTOP      = 0xE0, 0x28 /*0xE8*/, // Web Stop
	PS2_KEY_WEBFORWARD   = 0xE0, 0x30 /*0xE9*/, // Web Forward
	PS2_KEY_WEBBACK      = 0xE0, 0x38 /*0xEA*/, // Web Back
	PS2_KEY_MYCOMPUTER   = 0xE0, 0x40 /*0xEB*/, // My Computer
	PS2_KEY_MAIL         = 0xE0, 0x48 /*0xEC*/, // Mail
	PS2_KEY_MEDIASELECT  = 0xE0, 0x50 /*0xED*/   // Media Select
#endif
};

/*
 https://zx-pk.ru/threads/33211-universalnyj-kontroller-ps-2-klaviatury-dlya-kompyuterov-s-matrichnymi-klaviaturami.html
 Xrust PS/2 Keyboard Controller mappings:
  Code    Key   ZX Key
  66      BKSP  [BS]
  (E0),6B LEFT  [Left]
  (E0),72 DOWN  [Down]
  (E0),75 UP    [Up]
  (E0),74 RIGHT [Right]
  76      ESC   [Edit]
  58      CAPS  [Caps lock]
  0D      TAB   [Ext mode]
  41      ,     [,]
  49      .     [.]
  4C      ;     [;]
  52      '     ["]

 Note:
  With Xrust controller firmware, keys with multi-byte scan codes 
  handled as keys with single-byte scan codes because of 0xE0 leading byte ignored:
  
  PS2_KEY_RCONTROL 0xE0, 0x14  "Right Control"              PS2_KEY_LCONTROL 0x14 "Left Control"
  PS2_KEY_DIVIDE   0xE0, 0x4A  "/ on numeric keypad"        PS2_KEY_SLASH    0x4A "/ on main keyboard"
  PS2_KEY_SYSRQ    0xE0, 0x12, "PrintScrn/SysRq"            PS2_KEY_LSHIFT   0x12 "Left Shift"
                   0xE0, 0x7C                               PS2_KEY_MULTIPLY 0x7C "* on numeric keypad"
  PS2_KEY_RMENU    0xE0, 0x11  "Right Alt"                  PS2_KEY_LMENU    0x11 "Left Alt"
  PS2_KEY_UP       0xE0, 0x75  "UpArrow on arrow keypad"    PS2_KEY_NUMPAD8  0x75
  PS2_KEY_LEFT     0xE0, 0x6B  "LeftArrow on arrow keypad"  PS2_KEY_NUMPAD4  0x6B
  PS2_KEY_RIGHT    0xE0, 0x74  "RightArrow on arrow keypad" PS2_KEY_NUMPAD6  0x74
  PS2_KEY_DOWN     0xE0, 0x72  "DownArrow on arrow keypad"  PS2_KEY_NUMPAD2  0x72
  PS2_KEY_DELETE   0xE0, 0x71  "Delete on arrow keypad"     PS2_KEY_DECIMAL  0x71 ". on numeric keypad"
*/

/*
 Taken from https://github.com/leonid-z/zxkeyboard/blob/master/zxps2adapter/zxps2adapter/main.asm

 1 byte scan code for press, prefix F0 for release
 <PS/2 Set 2 Scan Code>, <1st zx key code> <2nd zx key code>
*/
struct scan_code_table_t
{
	uint8_t ps_2_code;
	uint8_t zx_code_1;
	uint8_t zx_code_2;
}
const static scan_code_table[] = // ToDo: __not_in_flash("keys") if time critical
{
	{ PS2_KEY_TAB,        ZX_KEY_CS,   ZX_KEY_SS   }, // Tab -- EXTENDED MODE
	{ PS2_KEY_LSHIFT,     ZX_KEY_CS,   ZX_KEY_NONE }, // Left Shift -- CAPS SHIFT
//	{ PS2_KEY_LCONTROL,   ZX_KEY_SS,   ZX_KEY_NONE }, // Left Ctrl	
	{ PS2_KEY_Q,          ZX_KEY_Q,    ZX_KEY_NONE }, // Q
	{ PS2_KEY_1,          ZX_KEY_1,    ZX_KEY_NONE }, // 1
	{ PS2_KEY_Z,          ZX_KEY_Z,    ZX_KEY_NONE }, // Z
	{ PS2_KEY_S,          ZX_KEY_S,    ZX_KEY_NONE }, // S
	{ PS2_KEY_A,          ZX_KEY_A,    ZX_KEY_NONE }, // A
	{ PS2_KEY_W,          ZX_KEY_W,    ZX_KEY_NONE }, // W
	{ PS2_KEY_2,          ZX_KEY_2,    ZX_KEY_NONE }, // 2
	{ PS2_KEY_C,          ZX_KEY_C,    ZX_KEY_NONE }, // C
	{ PS2_KEY_X,          ZX_KEY_X,    ZX_KEY_NONE }, // X
	{ PS2_KEY_D,          ZX_KEY_D,    ZX_KEY_NONE }, // D
	{ PS2_KEY_E,          ZX_KEY_E,    ZX_KEY_NONE }, // E
	{ PS2_KEY_4,          ZX_KEY_4,    ZX_KEY_NONE }, // 4
	{ PS2_KEY_3,          ZX_KEY_3,    ZX_KEY_NONE }, // 3
	{ PS2_KEY_SPACE,      ZX_KEY_SP,   ZX_KEY_NONE }, // Space
	{ PS2_KEY_V,          ZX_KEY_V,    ZX_KEY_NONE }, // V
	{ PS2_KEY_F,          ZX_KEY_F,    ZX_KEY_NONE }, // F
	{ PS2_KEY_T,          ZX_KEY_T,    ZX_KEY_NONE }, // T
	{ PS2_KEY_R,          ZX_KEY_R,    ZX_KEY_NONE }, // R
	{ PS2_KEY_5,          ZX_KEY_5,    ZX_KEY_NONE }, // 5
	{ PS2_KEY_N,          ZX_KEY_N,    ZX_KEY_NONE }, // N
	{ PS2_KEY_B,          ZX_KEY_B,    ZX_KEY_NONE }, // B
	{ PS2_KEY_H,          ZX_KEY_H,    ZX_KEY_NONE }, // H
	{ PS2_KEY_G,          ZX_KEY_G,    ZX_KEY_NONE }, // G
	{ PS2_KEY_Y,          ZX_KEY_Y,    ZX_KEY_NONE }, // Y
	{ PS2_KEY_6,          ZX_KEY_6,    ZX_KEY_NONE }, // 6
	{ PS2_KEY_M,          ZX_KEY_M,    ZX_KEY_NONE }, // M
	{ PS2_KEY_J,          ZX_KEY_J,    ZX_KEY_NONE }, // J
	{ PS2_KEY_U,          ZX_KEY_U,    ZX_KEY_NONE }, // U
	{ PS2_KEY_7,          ZX_KEY_7,    ZX_KEY_NONE }, // 7
	{ PS2_KEY_8,          ZX_KEY_8,    ZX_KEY_NONE }, // 8
	{ PS2_KEY_COMMA,      ZX_KEY_SS,   ZX_KEY_N    }, // ,
	{ PS2_KEY_K,          ZX_KEY_K,    ZX_KEY_NONE }, // K
	{ PS2_KEY_I,          ZX_KEY_I,    ZX_KEY_NONE }, // I
	{ PS2_KEY_O,          ZX_KEY_O,    ZX_KEY_NONE }, // O
	{ PS2_KEY_0,          ZX_KEY_0,    ZX_KEY_NONE }, // 0
	{ PS2_KEY_9,          ZX_KEY_9,    ZX_KEY_NONE }, // 9
	{ PS2_KEY_PERIOD,     ZX_KEY_SS,   ZX_KEY_M    }, // .
	{ PS2_KEY_SLASH,      ZX_KEY_SS,   ZX_KEY_V    }, // /
	{ PS2_KEY_L,          ZX_KEY_L,    ZX_KEY_NONE }, // L
	{ PS2_KEY_SEMICOLON,  ZX_KEY_SS,   ZX_KEY_O    }, // ;
	{ PS2_KEY_P,          ZX_KEY_P,    ZX_KEY_NONE }, // P
	{ PS2_KEY_MINUS,      ZX_KEY_SS,   ZX_KEY_J    }, // -
	{ PS2_KEY_APOSTROPHE, ZX_KEY_SS,   ZX_KEY_P    }, // "
	{ PS2_KEY_EQUALS,     ZX_KEY_SS,   ZX_KEY_L    }, // =
	{ PS2_KEY_CAPITAL,    ZX_KEY_CS,   ZX_KEY_2    }, // Caps Lock
	{ PS2_KEY_RSHIFT,     ZX_KEY_SS,   ZX_KEY_NONE }, // Right Shift -- SYMBOL SHIFT
	{ PS2_KEY_RETURN,     ZX_KEY_ENT,  ZX_KEY_NONE }, // Enter
	{ PS2_KEY_BACK,       ZX_KEY_CS,   ZX_KEY_0    }, // BackSpace - DELETE
	{ PS2_KEY_NUMPAD1,    ZX_KEY_1,    ZX_KEY_NONE }, // 1
	{ PS2_KEY_NUMPAD4,    ZX_KEY_4,    ZX_KEY_NONE }, // 4
	{ PS2_KEY_NUMPAD7,    ZX_KEY_7,    ZX_KEY_NONE }, // 7
	{ PS2_KEY_NUMPAD0,    ZX_KEY_0,    ZX_KEY_NONE }, // 0
	{ PS2_KEY_DECIMAL,    ZX_KEY_SS,   ZX_KEY_M    }, // .
	{ PS2_KEY_NUMPAD2,    ZX_KEY_2,    ZX_KEY_NONE }, // 2
	{ PS2_KEY_NUMPAD5,    ZX_KEY_5,    ZX_KEY_NONE }, // 5
	{ PS2_KEY_NUMPAD6,    ZX_KEY_6,    ZX_KEY_NONE }, // 6
	{ PS2_KEY_NUMPAD8,    ZX_KEY_8,    ZX_KEY_NONE }, // 8
	{ PS2_KEY_ESCAPE,     ZX_KEY_CS,   ZX_KEY_1    }, // Esc - EDIT
	{ PS2_KEY_ADD,        ZX_KEY_SS,   ZX_KEY_K    }, // +
	{ PS2_KEY_NUMPAD3,    ZX_KEY_3,    ZX_KEY_NONE }, // 3
	{ PS2_KEY_SUBTRACT,   ZX_KEY_SS,   ZX_KEY_J    }, // -
	{ PS2_KEY_MULTIPLY,   ZX_KEY_SS,   ZX_KEY_B    }, // *
	{ PS2_KEY_NUMPAD9,    ZX_KEY_9,    ZX_KEY_NONE }, // 9
	{ 0x00, 0x00, 0x00 }
},
// Mapping table for PS/2 set 2 extended scan codes starting with 0xE0.
// Single-byte make code used instead of two byte codes
// in attempt to make things faster and simpler.  
scan_code_table_E0[] =
{
//	{ PS2_KEY_LCONTROL /*PS2_KEY_RCONTROL*/,  ZX_KEY_SS,   ZX_KEY_NONE }, // Left Ctrl
	{ PS2_KEY_NUMPAD8 /*PS2_KEY_UP*/,         ZX_KEY_CS,   ZX_KEY_7}, // Up
	{ PS2_KEY_NUMPAD4 /*PS2_KEY_LEFT*/,       ZX_KEY_CS,   ZX_KEY_5}, // Left
	{ PS2_KEY_NUMPAD2 /*PS2_KEY_DOWN*/,       ZX_KEY_CS,   ZX_KEY_6}, // Down
	{ PS2_KEY_NUMPAD6 /*PS2_KEY_RIGHT*/,      ZX_KEY_CS,   ZX_KEY_8}, // Right
	{ PS2_KEY_SLASH /*PS2_KEY_DIVIDE*/,       ZX_KEY_SS,   ZX_KEY_V}, // /
	{ PS2_KEY_RETURN /*PS2_KEY_NUMPADENTER*/, ZX_KEY_ENT,  ZX_KEY_NONE}, // Enter
	{ PS2_KEY_NUMPAD9 /*PS2_KEY_PRIOR*/,      ZX_KEY_CS,   ZX_KEY_3 }, // PageUp
	{ PS2_KEY_NUMPAD3 /*PS2_KEY_NEXT*/,       ZX_KEY_CS,   ZX_KEY_4 }, // PageDown	
	{ 0x00, 0x00, 0x00 }
};

void zx_keyboard_init()
{
	CH446Q_init();
	CH446Q_reset();
}

// Returns true if CTRL pressed.
bool osd_buttons_update(uint8_t make_code, bool state)
{
	static bool CTRL, LEFT, RIGHT, DOWN = false;

	// Single-byte make code used instead of extended two byte codes, prefix 0xE0 ignored.
	if(make_code == PS2_KEY_LCONTROL) // PS2_KEY_RCONTROL
		CTRL = state;
	if(make_code == PS2_KEY_NUMPAD4) // PS2_KEY_LEFT
		LEFT = state;
	if(make_code == PS2_KEY_NUMPAD6) // PS2_KEY_RIGHT
		RIGHT = state;
	if(make_code == PS2_KEY_NUMPAD2) // PS2_KEY_DOWN
		DOWN = state;
	if(make_code == PS2_KEY_NUMPAD8) // PS2_KEY_UP
	{
		// Special key combination for move to parent directory.
		LEFT = state;
		RIGHT = state;
	}

	const uint8_t buttons = CTRL * ((OSD_BUTTON_SELECT * DOWN) | (OSD_BUTTON_RIGHT * RIGHT) | (OSD_BUTTON_LEFT * LEFT));

	set_osd_buttons(buttons);

	return CTRL;
}

void special_keys_update(uint8_t make_code, bool state)
{
	static bool PAUSE = false;

	if(make_code == PS2_KEY_F12)
		CH446Q_set(ZX_KEY_RESET, state); // Reset signal until key released.

	if(make_code == PS2_KEY_F11)
		CH446Q_set(ZX_KEY_MAGIC, state); // Magic/NMI signal until key released.

	if(make_code == PS2_KEY_F10)
	{
		if(state)
			PAUSE = !PAUSE; // Key press toggles pause state.

		CH446Q_set(ZX_KEY_PAUSE, PAUSE);
	}
}

void zx_keyboard_update()
{
	uint8_t len, code_0, code_1, code_2;

	while(len = ps2_get_raw_code(&code_0, &code_1, &code_2))
	{
		const uint8_t make_code = (len == 1) ? code_0 : (len == 2) ? code_1 : code_2;
		const uint8_t state_code = (len == 3) ? code_1 : code_0;
		const bool state = (state_code != 0xF0);

		if(osd_buttons_update(make_code, state)) // If CTRL modifier key pressed.
		{
			if(state) // Process ZX keyboard keys release but not press.
				continue; // Do not process ZX keyboard and special keys if CTRL modifier key pressed.
		}

		const struct scan_code_table_t *entry = (code_0 == 0xE0) ? scan_code_table_E0 : scan_code_table;

		// Linear find with O(n). Can be pre-sorted offline and processes with O(log2n),
		// or sparsed array can be created with at least 0x7F(0x83) entries for O(1).
		while (entry->ps_2_code)
		{
			if(entry->ps_2_code == make_code)
			{
				if(entry->zx_code_1 != ZX_KEY_NONE)
					CH446Q_set(entry->zx_code_1, state);

				if(entry->zx_code_2 != ZX_KEY_NONE)
					CH446Q_set(entry->zx_code_2, state); // Extended 58-key keyboard key.

				break;
			}
			entry++;
		}

		special_keys_update(make_code, state);
	}
}