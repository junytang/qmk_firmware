/* Copyright 2023 Colin Lam (Ploopy Corporation)
 * Copyright 2020 Christopher Courtney, aka Drashna Jael're  (@drashna) <drashna@live.com>
 * Copyright 2019 Sunjun Kim
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include QMK_KEYBOARD_H

#include "../../ploopyco.h"
#include "../../tap_dance.h"

#include "drivers/sensors/pmw33xx_common.h"

#include "rgblight.h"
#include "printf.h"

#include "../../../common/tmag5273wheel.h"

#include "pointing_device_gestures.h"

#include "math.h"

/* State variables for wheels. */
uint16_t leftwheel_deadzone_center = 0;
uint16_t rightwheel_deadzone_center = 0;

uint16_t leftwheel_current_position = 0;
uint16_t rightwheel_current_position = 0;
uint32_t last_scroll_time = 0;

/* Layer lighting, used to blink when making gestures. */
rgblight_segment_t righty_nav_layer_colour[] = RGBLIGHT_LAYER_SEGMENTS( {0, 2, HSV_NAVBLUE} );
rgblight_segment_t lefty_nav_layer_colour[]  = RGBLIGHT_LAYER_SEGMENTS( {0, 2, HSV_NAVGREEN} );
const rgblight_segment_t PROGMEM control_layer_colour[]        = RGBLIGHT_LAYER_SEGMENTS( {0, 2, HSV_RED} );
const rgblight_segment_t PROGMEM gesture_layer_colour[]        = RGBLIGHT_LAYER_SEGMENTS( {0, 2, HSV_GESTUREYELLOW} );
const rgblight_segment_t PROGMEM option_changed_layer_colour[] = RGBLIGHT_LAYER_SEGMENTS( {0, 2, HSV_OPTIONCHANGED} );

/* Lighting layer definitions. Later layers take precedence, so these
   are defined in the order that they will be displayed in. */
const rgblight_segment_t* const PROGMEM my_rgb_layers[] = RGBLIGHT_LAYERS_LIST(
    righty_nav_layer_colour,
    lefty_nav_layer_colour,
    control_layer_colour,
    gesture_layer_colour,
    option_changed_layer_colour
);

/* Colour layer names. */
enum {
    RIGHTY_NAV_LAYER_COLOUR = 0,
    LEFTY_NAV_LAYER_COLOUR,
    CONTROL_LAYER_COLOUR,
    GESTURE_LAYER_COLOUR,
    OPTION_CHANGED_LAYER_COLOUR
};

  /* EEPROM memory for the various persistent states that the A+ can have.
     See eeconfig_init_user() for default values and meanings. The struct is
     defined in gesture_mode.h. */
user_config_t user_config;

/* --- VIA-tunable scroll-acceleration config (handlers at EOF, via_custom_value_command_kb) --- */
typedef struct {
    bool  enabled : 1;  /* master switch for curve acceleration (1 = on). */
    float offset;       /* per-tick |wheel_delta| where boost is halfway; larger = engages later / calmer low end. */
    float takeoff;      /* transition steepness (1/width); larger = sharper knee. Mirrors pointing_device_accel takeoff. */
    float growth_rate;  /* saturation rate toward limit_upper; growth_rate/takeoff (g/k) shapes how fast the top is reached. Mirrors accel growth_rate. */
    float limit;        /* low-speed floor factor (<1.0 = finer "creep"; 1.0 = 1:1 at rest). Mirrors accel limit. */
    float limit_upper;  /* top scroll-rate multiplier vs the base divisor. Mirrors accel LIMIT_UPPER. */
} scroll_accel_config_t;

/* DPI config: 5 user-settable presets plus the currently-active preset index.
   Stored raw (actual CPI values), exposed to the web UI as real DPI numbers. */
typedef struct {
    uint16_t presets[5];
    uint8_t  active_preset;
} dpi_config_t;

/* Legal range / default for the VIA auto-sleep controls (defined up here
   because both the global initializer below and the init guard use them).
   The timeout max is > 255, so the aplus.json "options" range is encoded by
   VIA as 2 big-endian bytes -- sleep_config_set_value / _get_value must match
   that byte order. The enabled toggle is a plain 1-byte 0/1.
   0 is excluded from the timeout range: it is the EEPROM-uninitialized
   sentinel, and "never sleep" is expressed with enabled=0 instead. */
#define SLEEP_UI_MIN_MIN 1
#define SLEEP_UI_MAX_MIN 1440 /* 24 h */
#define SLEEP_DEFAULT_ENABLED 1 /* factory default: auto-sleep on */

/* Auto-sleep config: master on/off switch plus idle timeout in minutes
   before the trackball sleeps.
   timeout_min is stored raw as a minute count; SLEEP_UI_MIN_MIN..
   SLEEP_UI_MAX_MIN are the only legal values, so 0 (all-zero /
   invalid-datablock read) and values above the max (erased 0xFFFF) are both
   caught as uninitialized/corrupt by the guard in keyboard_post_init_user.
   0 is deliberately NOT offered as a "never sleep" value in the VIA menu: it
   must stay reserved as the uninitialized sentinel (same convention as
   g_scroll_accel_config); "never sleep" is what enabled=0 is for.
   enabled is 0/1; any other byte value (e.g. an erased tail) is reseeded by
   the same guard.
   NOTE: the trailing uint8_t pads the struct to 4 bytes, which puts the end
   of this block at 48. The drag-scroll config is appended after it, and the
   total lands at exactly EECONFIG_USER_DATA_SIZE (64). If anything grows,
   bump it in rp2040zero/config.h -- the _Static_assert below fails loudly. */
typedef struct {
    uint16_t timeout_min;
    uint8_t  enabled;
} sleep_config_t;

/* Drag-scroll divisors: mouse x/y counts consumed per scroll tick. Mirrors
   PLOOPY_DRAGSCROLL_DIVISOR_H/V but is runtime-tunable (VIA DragScroll
   section) and persisted as the last block of the EECONFIG user datablock. */
typedef struct {
    float h;
    float v;
} drag_scroll_config_t;

drag_scroll_config_t g_drag_scroll_config = {
    .h = PLOOPY_DRAGSCROLL_DIVISOR_H,
    .v = PLOOPY_DRAGSCROLL_DIVISOR_V,
};

/* LED config: 保存左手和右手模式下的指示灯颜色 (Hue & Sat) */
typedef struct {
    uint8_t righty_hue;
    uint8_t righty_sat;
    uint8_t lefty_hue;
    uint8_t lefty_sat;
} led_config_t;

led_config_t g_led_config = {
    .righty_hue = 180, // 默认 HSV_NAVBLUE 的 Hue
    .righty_sat = 220,
    .lefty_hue  = 66,  // 默认 HSV_NAVGREEN 的 Hue
    .lefty_sat  = 240,
};

scroll_accel_config_t g_scroll_accel_config = {
    .enabled = 1,
    .offset = 220.0f,
    .takeoff = 0.03f,
    .growth_rate = 0.012f,
    .limit = 1.3f,
    .limit_upper = 5.0f,
};

dpi_config_t g_dpi_config = {
    .presets = { 800, 1200, 1600, 2000, 2400 },
    .active_preset = 1,
};

sleep_config_t g_sleep_config = {
    .timeout_min = TRACKBALL_SLEEP_DEFAULT_TIMEOUT_MIN,
    .enabled     = SLEEP_DEFAULT_ENABLED,
};

/* All persistent structs live in the EECONFIG user-data block
   (EECONFIG_USER_DATA_SIZE, config.h), via eeconfig_read_user_datablock /
   eeconfig_update_user_datablock -- matching users/junytang/ploopy_via.c.
   Once EECONFIG_USER_DATA_SIZE > 0 the legacy eeconfig_read_user /
   eeconfig_update_user are unavailable, so user_config and the scroll config
   must both use the datablock API. user_config occupies the first bytes; the
   scroll config follows it, then the DPI config, then the sleep config, then
   the drag-scroll config.
   Appending (never re-ordering) keeps existing EEPROM contents valid when a
   new struct is added: only the fresh tail bytes change meaning, and the
   init guards reseed them. This block is separate from the VIA custom-config
   region, so it does not collide with the drashna/pointing_device_accel
   module. */
#define USER_CONFIG_EEPROM_OFFSET 0
#define SCROLL_ACCEL_EEPROM_OFFSET (USER_CONFIG_EEPROM_OFFSET + sizeof(user_config_t))
#define DPI_EEPROM_OFFSET (SCROLL_ACCEL_EEPROM_OFFSET + sizeof(scroll_accel_config_t))
#define SLEEP_EEPROM_OFFSET (DPI_EEPROM_OFFSET + sizeof(dpi_config_t))
#define DRAGSCROLL_EEPROM_OFFSET (SLEEP_EEPROM_OFFSET + sizeof(sleep_config_t))
#define LED_CONFIG_EEPROM_OFFSET (DRAGSCROLL_EEPROM_OFFSET + sizeof(drag_scroll_config_t))
_Static_assert(SCROLL_ACCEL_EEPROM_OFFSET + sizeof(scroll_accel_config_t) <= EECONFIG_USER_DATA_SIZE,
                "user_config + scroll_accel_config exceed EECONFIG_USER_DATA_SIZE; grow it in config.h");
_Static_assert(DPI_EEPROM_OFFSET + sizeof(dpi_config_t) <= EECONFIG_USER_DATA_SIZE,
                "user_config + scroll_accel_config + dpi_config exceed EECONFIG_USER_DATA_SIZE; grow it in config.h");
_Static_assert(SLEEP_EEPROM_OFFSET + sizeof(sleep_config_t) <= EECONFIG_USER_DATA_SIZE,
                "user_config + scroll_accel_config + dpi_config + sleep_config exceed EECONFIG_USER_DATA_SIZE; grow it in config.h");
_Static_assert(DRAGSCROLL_EEPROM_OFFSET + sizeof(drag_scroll_config_t) <= EECONFIG_USER_DATA_SIZE,
                "user_config + scroll_accel_config + dpi_config + sleep_config + drag_scroll_config exceed EECONFIG_USER_DATA_SIZE; grow it in config.h");
_Static_assert(LED_CONFIG_EEPROM_OFFSET + sizeof(led_config_t) <= EECONFIG_USER_DATA_SIZE,
                "user_config + ... + led_config exceed EECONFIG_USER_DATA_SIZE; grow it in config.h");

/* Per-param real ranges for VIA projection (must match aplus.json "options"). */
#define SCROLL_ACCEL_R_MIN_OFFSET 0.0f
#define SCROLL_ACCEL_R_MAX_OFFSET 400.0f
#define SCROLL_ACCEL_R_MIN_TAKEOFF 0.005f
#define SCROLL_ACCEL_R_MAX_TAKEOFF 0.1f
#define SCROLL_ACCEL_R_MIN_GROWTH_RATE 0.001f
#define SCROLL_ACCEL_R_MAX_GROWTH_RATE 0.1f
#define SCROLL_ACCEL_R_MIN_LIMIT 0.5f
#define SCROLL_ACCEL_R_MAX_LIMIT 2.0f   /* >1.0 = 低速比 1:1 更快（慢速滚动提速） */
#define SCROLL_ACCEL_R_MIN_LIMIT_UPPER 1.0f
#define SCROLL_ACCEL_R_MAX_LIMIT_UPPER 10.0f

/* Drag-scroll divisors (mouse x/y counts per scroll tick). Larger = slower.
   Exposed in the VIA "DragScroll" section scaled x10 (0.1..200.0). */
#define DRAGSCROLL_R_MIN 0.1f
#define DRAGSCROLL_R_MAX 200.0f
#define DRAGSCROLL_UI_SCALE 10

void scroll_accel_rebuild_lut(void);
static void scroll_accel_config_update(void);

static void sleep_config_update(void);
static void sleep_config_set_value(uint8_t *data);
static void sleep_config_get_value(uint8_t *data);

static void drag_scroll_config_update(void);
static void drag_scroll_apply(void);

static void led_config_update(void);
static void led_config_apply(void);

static void dpi_apply(void);
static void dpi_config_update(void);
static void dpi_config_set_value(uint8_t *data);
static void dpi_config_get_value(uint8_t *data);

/* Add custom keycodes for control layer. */
enum my_keycodes {
  PKC_TGL_MIRROR = SAFE_RANGE,
  PKC_TGL_VERT_SCRL,
  PKC_TGL_HORIZ_SCRL,
  PKC_TGL_DRAG_SCRL,
  PKC_GESTURE,
  PKC_DRAG_SCROLL,
  PKC_ADJUST_LED_BRIGHTNESS,
  PKC_BLINKY_DPI_CONFIG
};

// /* Layer names. Declared in gesture_mode.h  */
// enum {
//     LAYER_NAV_RIGHT_HANDED = 0,
//     LAYER_NAV_LEFT_HANDED,
//     LAYER_CONTROL
// };

/* ==========================================================================
 * 手势方向与 via.json 虚拟矩阵 (Row 1) 列号的映射转换表:
 * Ploopy 官方手势方向定义 (0~7 从正东顺时针):
 *   0: East (正东/右 →)       -> via.json 列号 3 (1,3)
 *   1: South-East (东南/右下 ↘) -> via.json 列号 4 (1,4)
 *   2: South (正南/下 ↓)       -> via.json 列号 5 (1,5)
 *   3: South-West (西南/左下 ↙) -> via.json 列号 6 (1,6)
 *   4: West (正西/左 ←)       -> via.json 列号 7 (1,7)
 *   5: North-West (西北/左上 ↖) -> via.json 列号 0 (1,0)
 *   6: North (正北/上 ↑)       -> via.json 列号 1 (1,1)
 *   7: North-East (东北/右上 ↗) -> via.json 列号 2 (1,2)
 * ========================================================================== */
#define GESTURE_MATRIX_ROW 1
static const uint8_t PROGMEM gesture_dir_to_matrix_col[8] = { 3, 4, 5, 6, 7, 0, 1, 2 };

/* Send a VIA-configured gesture action. A keycode in the QK_MACRO range must
   run the matching VIA dynamic macro; tap_code16() only emits HID keys and
   would do nothing for M0-M15. */
static void gesture_send_configured(uint16_t keycode) {
#ifdef DYNAMIC_KEYMAP_ENABLE
    if (keycode >= QK_MACRO && keycode <= QK_MACRO_MAX) {
        uint8_t id = (uint8_t)(keycode - QK_MACRO);
        if (id < DYNAMIC_KEYMAP_MACRO_COUNT) {
            dynamic_keymap_macro_send(id);
        }
        return;
    }
#endif
    tap_code16(keycode);
}

/* Override default function in submodule code so that we can blink whenever
   a successful gesture is registered. */
void pointing_device_gestures_trigger(uint8_t direction) {

    /* 优先读取 VIA 网页中为该手势配置的动态按键 */
    if (direction < 8) {
        uint8_t col = pgm_read_byte(&gesture_dir_to_matrix_col[direction]);
        uint8_t layer = get_highest_layer(layer_state);
        uint16_t mapped_kc = dynamic_keymap_get_keycode(layer, GESTURE_MATRIX_ROW, col);

        /* 高层的虚拟手势键通常没有填写（KC_NO）或为透明键（KC_TRNS）；
           两者都应回退到第 0 基础层。只判断 KC_TRNS 会让左/右手层
           （LAYER_NAV_* 常驻）把基础层的设置遮住。 */
        if (mapped_kc == KC_NO || mapped_kc == KC_TRNS) {
            mapped_kc = dynamic_keymap_get_keycode(0, GESTURE_MATRIX_ROW, col);
        }

        // 如果 VIA 填写了有效按键 (既不是 KC_NO 也不是 KC_TRNS)
        if (mapped_kc != KC_NO && mapped_kc != KC_TRNS) {
            gesture_send_configured(mapped_kc); // 触发 VIA 中设定的按键/快捷键/宏
            rgblight_blink_layer(OPTION_CHANGED_LAYER_COLOUR, OPTION_CHANGE_BLINK_TIMEOUT * 2);
            return; // 成功执行，直接退出，不再走后面的默认逻辑
        }
    }

    /* 如果 VIA 没有配置任何按键，回退执行以下原生逻辑 */
    /* Directions are numbered off 0-7, starting with east. */
    switch( direction ) {
        case 0: /* East, Next Virtual Desktop. */
            /* On Windows, send CTRL + LGUI + RIGHT. */
            if ( detected_host_os() == OS_WINDOWS ) {
                tap_code16( LCG( KC_RIGHT ) );
            }
            /* On Linux, send CTRL + ALT + RIGHT. */
            else if( detected_host_os() == OS_LINUX ) {
                tap_code16( LCA( KC_RIGHT ) );
            }
            /* On MacOs, send Control + RIGHT. */
            else {
                tap_code16( LCTL( KC_RIGHT ) );
            }
            break;
        case 1: /* South-East, Paste. */
            /* On Windows/Linux, send CTRL + V. On MacOs, send CMD + V. */
            if ( detected_host_os() != OS_MACOS ) {
                tap_code16( C(KC_V) );
            } else {
                tap_code16( G(KC_V) );
            }
            break;
        case 2: /* South, Copy. */
            /* On Windows/Linux, send CTRL + C. On MacOs, send CMD + C. */
            if ( detected_host_os() != OS_MACOS ) {
                tap_code16( C(KC_C) );
            } else {
                tap_code16( G(KC_C) );
            }
            break;
        case 3: /* South-West, Cut. */
            /* On Windows/Linux, send CTRL + X. On MacOs, send CMD + X. */
            if ( detected_host_os() != OS_MACOS ) {
                tap_code16( C(KC_X) );
            } else {
                tap_code16( G(KC_X) );
            }
            break;
        case 4: /* West, Previous Virtual Desktop. */
            /* On Windows, send CTRL + LGUI + LEFT. */
            if ( detected_host_os() == OS_WINDOWS ) {
                tap_code16( LCG( KC_LEFT ) );
            }
            /* On Linux, send CTRL + ALT + LEFT. */
            else if( detected_host_os() == OS_LINUX ) {
                tap_code16( LCA( KC_LEFT ) );
            }
            /* On MacOs, send Control + LEFT. */
            else {
                tap_code16( LCTL( KC_LEFT ) );
            }
            break;
        case 5: /* North-West, Undo. */
            /* On Windows/Linux, send CTRL + Z. On MacOs, send CMD + Z. */
            if ( detected_host_os() != OS_MACOS ) {
                tap_code16( C(KC_Z) );
            } else {
                tap_code16( G(KC_Z) );
            }
            break;
        case 6: /* North, Play/Pause Audio. */
            tap_code16( KC_MEDIA_PLAY_PAUSE );
            break;
        case 7: /* North-East, Redo. */
            /* On Windows/Linux, send CTRL + Y. */
            if ( detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX ) {
                tap_code16( C(KC_Y) );
            }
            /* On MacOs, send CMD + SHIFT + Z. */
            else {
                register_code(KC_LCMD);
                register_code(KC_LSFT);
                tap_code(KC_Z);
                unregister_code(KC_LCMD);
                unregister_code(KC_LSFT);
            }
            break;
        default:
            break;
    }

    /* Flash light to indicate successful gesture processing. */
    rgblight_blink_layer(OPTION_CHANGED_LAYER_COLOUR, OPTION_CHANGE_BLINK_TIMEOUT*2);
}

/* Mouse gestures keymap. Since we cannot use custom keycodes in this array and
   we've overridden the function that uses it, we define this here with
   KC_NO (no action) to prevent a compiler error. See pointing_device_gestures_trigger()
   for what the gestures actually do. */
const uint16_t PROGMEM pointing_device_gestures[NUM_GESTURE_DIRECTIONS] =
    GESTURES_CARDINAL_AND_ORDINAL_DIRECTIONS( KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO );

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [LAYER_NAV_RIGHT_HANDED] = LAYOUT_16(
        /* 第 0 行：8 个物理键 */
        MS_BTN6, MS_BTN5,                   TD(TD_DRAG_SCROLL), MS_BTN4,
        MS_BTN1, MS_BTN3,
        MS_BTN2, TD(TD_CONTROL_GESTURE),
        /* 第 1 行：8 个手势默认键 (默认全为 KC_NO) */
        KC_NO,   KC_NO,                     KC_NO,              KC_NO,
        KC_NO,   KC_NO,                     KC_NO,              KC_NO
    ),

    [LAYER_NAV_LEFT_HANDED] = LAYOUT_16(
        LT(3, MS_BTN4),         TD(TD_DRAG_SCROLL), MS_BTN5, MS_BTN6,
        MS_BTN3,                MS_BTN1,
        TD(TD_CONTROL_GESTURE), MS_BTN2,
        KC_NO,                  KC_NO,              KC_NO,   KC_NO,
        KC_NO,                  KC_NO,              KC_NO,   KC_NO
    ),

    [LAYER_CONTROL] = LAYOUT_16(
        PKC_BLINKY_DPI_CONFIG, PKC_ADJUST_LED_BRIGHTNESS, PKC_TGL_VERT_SCRL, PKC_TGL_HORIZ_SCRL,
        PKC_TGL_MIRROR,        PKC_TGL_DRAG_SCRL,
        MO(LAYER_CONTROL),     MO(LAYER_CONTROL),
        KC_NO,                 KC_NO,                     KC_NO,              KC_NO,
        KC_NO,                 KC_NO,                     KC_NO,              KC_NO
    ),

    [3] = LAYOUT_16(
        _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______
    ),
    [4] = LAYOUT_16(
        _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______
    ),
    [5] = LAYOUT_16(
        _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______
    ),
    [6] = LAYOUT_16(
        _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______
    ),
    [7] = LAYOUT_16(
        _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______
    )
};

/* 3-in-1 control/gesture key (TD_CONTROL_GESTURE), defined in tap_dance.c:
 *   single tap    -> user_config.gesture_tap_keycode (MS_BTN7 default, VIA-settable)
 *   single hold   -> pointing-device gesture mode
 *   tap then hold -> MO(LAYER_CONTROL) for the duration of the hold */
tap_dance_action_t tap_dance_actions[] = {
    [TD_CONTROL_GESTURE] = { .fn = { .on_each_tap       = cg_press,
                                     .on_dance_finished = cg_fin,
                                     .on_reset          = cg_rst },
                              .user_data = (void *)&user_config.gesture_tap_keycode },
    [TD_DRAG_SCROLL]     = { .fn = { .on_each_tap       = ds_press,
                                     .on_dance_finished = ds_fin,
                                     .on_reset          = ds_rst },
                              .user_data = (void *)&user_config.drag_scroll_tap_keycode },
};

/* Per-key tapping term: only the 3-in-1 key needs a longer window, everything
 * else keeps the default so the rest of the keymap is unaffected. */
uint16_t get_tapping_term(uint16_t keycode, keyrecord_t *record) {
    switch (keycode) {
        case TD(TD_CONTROL_GESTURE):
            return TD_CONTROL_GESTURE_TERM;
        case TD(TD_DRAG_SCROLL):
            return TD_DRAG_SCROLL_TERM;
        default:
            return TAPPING_TERM;
    }
}

/* Called whenever layers are modified. */
layer_state_t layer_state_set_user(layer_state_t state) {
    /* Set layer colours. */
    switch (get_highest_layer(state)) {
        case LAYER_NAV_RIGHT_HANDED:
            rgblight_set_layer_state(RIGHTY_NAV_LAYER_COLOUR, true);

            rgblight_set_layer_state(LEFTY_NAV_LAYER_COLOUR, false);
            rgblight_set_layer_state(CONTROL_LAYER_COLOUR, false);

            dprintf("change layer: right-handed nav\n");
            break;
        case LAYER_NAV_LEFT_HANDED:
            rgblight_set_layer_state(LEFTY_NAV_LAYER_COLOUR, true);

            rgblight_set_layer_state(RIGHTY_NAV_LAYER_COLOUR, false);
            rgblight_set_layer_state(CONTROL_LAYER_COLOUR, false);

            dprintf("change layer: left-handed nav\n");
            break;
        case LAYER_CONTROL:
            rgblight_set_layer_state(CONTROL_LAYER_COLOUR, true);

            rgblight_set_layer_state(RIGHTY_NAV_LAYER_COLOUR, false);
            rgblight_set_layer_state(LEFTY_NAV_LAYER_COLOUR, false);

            dprintf("change layer: control\n");
            break;
        default:
            rgblight_setrgb(RGB_CYAN);
            dprintf("change layer: oopsie, invalid layer!\n");
            break;
    }

    return state;
}

/* Set sane defaults when EEPROM is reset. */
void eeconfig_init_user(void) {

    user_config.raw = 0;

    user_config.left_handed = false;               // false = right-handed (default); true = lefty nav mode.
    user_config.horizontal_scroll_arrows = false;   // False = Scroll events, True = left/right arrows.
    user_config.vertical_scroll_arrows = false;     // False = Scroll events, True = up/down arrows.
    user_config.drag_scroll_mode = false;           // Hold to activate.
    user_config.left_knob_invert = false;           // Knob direction not inverted.
    user_config.right_knob_invert = false;
    user_config.led_brightness = 1;                 // Dimmest visible level by default (0 = off).
    user_config.gesture_tap_keycode = MS_BTN7;      // Tap of the gesture key.
    user_config.drag_scroll_tap_keycode = MS_BTN8;  // Tap of the drag-scroll key.

    eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));

    /* Persist the scroll-accel defaults into the EECONFIG user-data block.
       Set them explicitly here (not only via the global initializer) so a
       factory-init / wipe can never leave k or max_boost as 0. */
    g_scroll_accel_config = (scroll_accel_config_t){
        .enabled = 1,
        .offset = 220.0f,
        .takeoff = 0.03f,
        .growth_rate = 0.012f,
        .limit = 1.3f,
        .limit_upper = 5.0f,
    };
    scroll_accel_config_update();

    /* Seed DPI presets + active preset into the EECONFIG user-data block too. */
    g_dpi_config = (dpi_config_t){
        .presets = { 800, 1200, 1600, 2000, 2400 },
        .active_preset = 1,
    };
    dpi_config_update();

    /* Seed the auto-sleep config (on/off + minutes) too, alongside the
       other blocks. */
    g_sleep_config = (sleep_config_t){
        .timeout_min = TRACKBALL_SLEEP_DEFAULT_TIMEOUT_MIN,
        .enabled     = SLEEP_DEFAULT_ENABLED,
    };
    sleep_config_update();

    /* Seed the drag-scroll divisors (the compile-time macro defaults) so a
       factory wipe never leaves them at 0 (division by zero in the scroll
       accumulator) or all-0xFF garbage. */
    g_drag_scroll_config = (drag_scroll_config_t){
        .h = PLOOPY_DRAGSCROLL_DIVISOR_H,
        .v = PLOOPY_DRAGSCROLL_DIVISOR_V,
    };
    drag_scroll_config_update();
    drag_scroll_apply();

    /* Seed led color config */
    g_led_config = (led_config_t){
        .righty_hue = 180,
        .righty_sat = 220,
        .lefty_hue  = 66,
        .lefty_sat  = 240,
    };
    led_config_update();
    led_config_apply();
}

void keyboard_pre_init_user(void) {

    /* Set up GPIO for both TMAG sensors. */
    gpio_set_pin_output_push_pull(TMAG5273_D0_PWR_PIN);
    gpio_set_pin_output_push_pull(TMAG5273_D1_PWR_PIN);
    gpio_write_pin_low(TMAG5273_D0_PWR_PIN);
    gpio_write_pin_low(TMAG5273_D1_PWR_PIN);

}

static bool     s_trackball_asleep        = false;
static uint32_t s_trackball_last_activity = 0;
static bool     s_sleep_rgb_was_enabled   = false;

/* True once the PMW3360 has been truly shut down via REG_Shutdown. While off
   the sensor cannot report motion, so only a key press (or the existing wake
   paths) can power it back on. */
static bool     s_pmw_off                 = false;

/* Set on wake. The stored wheel positions are stale because the wheels were
   not polled while asleep, so the next delta would be measured against where
   they were minutes ago and become one huge burst of scroll. */
static bool s_wheel_resync_pending = false;

/* Background sampling of PMW3360 SQUAL / shutter / pixel stats for the VIA
   Sensor menu. Toggle ON (VIA): reset the accumulators and sample every
   SENSOR_SAMPLE_MS in housekeeping_task_user while the sensor is awake.
   Toggle OFF: freeze; GET returns the session average. The web ranges are
   hidden via showIf while sampling is on (VIA only fetches custom values
   on connect, so the averages appear after the page is reloaded with the
   toggle off). */
#define SENSOR_SAMPLE_MS 500
static bool     s_sensor_sampling  = false;
static uint64_t s_squal_sum       = 0;
static uint64_t s_shutter_sum     = 0;
static uint64_t s_pixsum_sum      = 0; /* REG_Raw_Data_Sum 0x08 */
static uint64_t s_max_sum         = 0; /* REG_Maximum_Raw_data 0x09 */
static uint64_t s_min_sum         = 0; /* REG_Minimum_Raw_data 0x0a */
static uint32_t s_sample_count    = 0;
static uint32_t s_sensor_sample_last = 0;

/* Put the trackball to sleep: freeze the cursor, blank the underglow, truly
   power down the PMW3360 (REG_Shutdown) and cut the two TMAG5273 wheels from
   their power pins. Idempotent -- safe to call when already asleep. */
static void trackball_sleep_now(void) {
    if (s_trackball_asleep) return;
    s_trackball_asleep = true;
    s_sleep_rgb_was_enabled = rgblight_is_enabled();
    rgblight_disable_noeeprom();

    /* Truly power down the PMW3360. Writing 0xb6 to REG_Shutdown puts the
       sensor into its lowest-power state (the SPI interface still responds,
       but the optical array / DSP are off). This is the real power saving;
       gating the report alone would leave the sensor fully powered. */
    if (!s_pmw_off) {
        pmw33xx_write(0, REG_Shutdown, 0xb6);
        s_pmw_off = true;
    }

    /* Cut power to the two TMAG5273 wheels entirely via their power pins.
       While asleep, pointing_device_task_user returns before the wheel
       block, so no I2C traffic touches the unpowered devices. */
    gpio_write_pin_low(TMAG5273_D0_PWR_PIN);
    gpio_write_pin_low(TMAG5273_D1_PWR_PIN);
}

/* Wake the trackball from sleep: re-power and re-init the PMW3360 and both
   TMAG5273 wheels, and flag a wheel-position resync. The first poll after
   waking re-baselines current_position / deadzone_center against the new
   absolute angles, so no scroll burst occurs. */
static void trackball_wake_now(void) {
    bool was_asleep = s_trackball_asleep;
    s_trackball_asleep       = false;
    s_trackball_last_activity = timer_read32();
    if (was_asleep) {
        s_wheel_resync_pending = true;
        if (s_sleep_rgb_was_enabled && !rgblight_is_enabled()) {
            rgblight_enable_noeeprom();
        }

        /* Power the PMW3360 back on. REG_Shutdown (0xb6) clears the SROM, so
           we must run the full power-up reset + re-init sequence. The reset
           write alone is not enough; pmw33xx_init() re-uploads the firmware,
           restores CPI from the build default, then dpi_apply() re-applies the
           user's saved DPI preset. */
        if (s_pmw_off) {
            pmw33xx_write(0, REG_Power_Up_Reset, 0x5a);
            wait_ms(50);
            pmw33xx_init(0);
            dpi_apply();
            s_pmw_off = false;
        }

        /* Re-apply power to the two TMAG5273 wheels and re-init them (their
           power was cut on sleep). The wheel position resync below then
           re-baselines current_position / deadzone_center against the new
           absolute angle on the first poll, so no scroll burst occurs. */
        gpio_write_pin_high(TMAG5273_D0_PWR_PIN);
        wait_us(370);  // TMAG tstart_power_up, plus 100us margin
        tmag5273_init_device(TMAG5273_D0_I2C_ADDRESS);
        wait_ms(20);
        gpio_write_pin_high(TMAG5273_D1_PWR_PIN);
        wait_us(370);  // TMAG tstart_power_up, plus 100us margin
        tmag5273_init_device(TMAG5273_D1_I2C_ADDRESS);
        wait_ms(20);
    }
}

/* Called by process_record_user on any key event. */
static void trackball_wake(void) {
    trackball_wake_now();
}

// 计算 PMW3360 综合成像质量得分 (0 ~ 100 分)
uint8_t calculate_pmw3360_quality_score(uint8_t squal, uint16_t shutter,
                                        uint8_t max_px, uint8_t min_px, uint8_t pixel_sum) {
    // 1. SQUAL 得分 (满分 40)
    float s_squal = squal * 0.4f;
    if (s_squal > 40.0f) s_squal = 40.0f;

    // 2. 对比度得分 (满分 25)
    int16_t delta = max_px - min_px;
    float s_contrast = delta * 0.25f;
    if (s_contrast > 25.0f) s_contrast = 25.0f;
    if (max_px >= 127) s_contrast -= 3.0f; // 高光过曝惩罚
    if (min_px == 0)   s_contrast -= 3.0f; // 边缘死黑惩罚
    if (s_contrast < 0.0f) s_contrast = 0.0f;

    // 3. 曝光均衡得分 (满分 15)
    float dev = (float)abs((int16_t)pixel_sum - 50);
    float s_exp = 15.0f - dev * 0.5f;
    if (s_exp < 0.0f) s_exp = 0.0f;

    // 4. 快门健康得分 (满分 20)
    float s_shutter = 20.0f;
    if (shutter < 50) {
        s_shutter = 20.0f - (50 - shutter) * 0.2f;
    } else if (shutter > 400) {
        s_shutter = 20.0f - (shutter - 400) * 0.02f;
    }
    if (s_shutter < 0.0f) s_shutter = 0.0f;

    // 综合加总
    float total = s_squal + s_contrast + s_exp + s_shutter;
    if (total > 100.0f) total = 100.0f;
    if (total < 0.0f) total = 0.0f;

    return (uint8_t)(total + 0.5f); // 四舍五入
}

void housekeeping_task_user(void) {
    /* Sensor diagnostics: sample SQUAL + shutter + pixel stats while the VIA
       toggle is on and the PMW is awake. Runs before the sleep early-return
       so sampling still works when auto-sleep is disabled. */
    if (s_sensor_sampling && !s_trackball_asleep && !s_pmw_off) {
        if (timer_elapsed32(s_sensor_sample_last) >= SENSOR_SAMPLE_MS) {
            s_sensor_sample_last = timer_read32();
            s_squal_sum   += pmw33xx_read(0, REG_SQUAL);
            s_shutter_sum += ((uint16_t)pmw33xx_read(0, REG_Shutter_Upper) << 8) |
                              pmw33xx_read(0, REG_Shutter_Lower);
            s_pixsum_sum  += pmw33xx_read(0, REG_Raw_Data_Sum);
            s_max_sum     += pmw33xx_read(0, REG_Maximum_Raw_data);
            s_min_sum     += pmw33xx_read(0, REG_Minimum_Raw_data);
            s_sample_count++;
        }
    }

    /* The auto-sleep switch (g_sleep_config.enabled, VIA-settable) gates the
       idle timer entirely; the timeout comes from g_sleep_config.timeout_min
       (minutes, persisted in the EECONFIG user-data block). The compile-time
       TRACKBALL_SLEEP_DEFAULT_TIMEOUT_MIN only seeds it on EEPROM init.
       (uint32_t)min * 60000 cannot overflow for the legal 1..1440 range. */
    if (!g_sleep_config.enabled || s_trackball_asleep) {
        return;
    }
    uint32_t timeout_ms = (uint32_t)g_sleep_config.timeout_min * 60u * 1000u;
    if (timeout_ms > 0 && timer_elapsed32(s_trackball_last_activity) > timeout_ms) {
        trackball_sleep_now();
    }
}

/* Host (e.g. Mac) sleep: USB suspend. Power the trackball down too, so it is
   not left running while the computer is off. */
void suspend_power_down_user(void) {
    trackball_sleep_now();
}

void suspend_wakeup_init_user(void) {
    if (s_trackball_asleep) {
        rgblight_disable_noeeprom();
    }
}

void keyboard_post_init_user(void) {
    /* ============================ Debug Setup ============================ */

    /* If you turn these on, also uncomment CONSOLE_ENABLE = yes in
       post_rules.mk! */
    // debug_enable = true;
    // debug_matrix=true;
    // debug_keyboard=true;
    // debug_mouse=true;

    /* ========================== Scroll Wheel Setup ======================= */

    /* Start up the two wheels. */
    tmag5273_init();

    /* Init TMAG5273 D0 (left wheel) */
    gpio_write_pin_high(TMAG5273_D0_PWR_PIN);
    wait_us(370);  // TMAG tstart_power_up, plus 100us margin
    tmag5273_init_device(TMAG5273_D0_I2C_ADDRESS);
    wait_ms(20);
    leftwheel_current_position = tmag5273_get_angle(TMAG5273_D0_I2C_ADDRESS);
    leftwheel_deadzone_center = leftwheel_current_position;


    /* Init TMAG5273 D1 (right wheel) */
    gpio_write_pin_high(TMAG5273_D1_PWR_PIN);
    wait_us(370);  // TMAG tstart_power_up, plus 100us margin
    tmag5273_init_device(TMAG5273_D1_I2C_ADDRESS);
    wait_ms(20);
    rightwheel_current_position = tmag5273_get_angle(TMAG5273_D1_I2C_ADDRESS);
    rightwheel_deadzone_center = rightwheel_current_position;

    /* =================== Persistent Configuration Setup ================== */

    /* Load up configuration details from the EEPROM user-data block. */
    eeconfig_read_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));
    /* An erased/garbage block reads as 0xFFFFFFFF (or a corrupt led_brightness);
       all-zero means the block was never written (both tap keycodes would be
       KC_NO and the drag/gesture taps would be dead) -- reseed the board
       defaults and persist them so the gesture keycode and lighting stay sane. */
    if (user_config.raw == 0xFFFFFFFFFFFFFFFFULL || user_config.raw == 0 ||
        user_config.led_brightness > 5) {
        user_config.raw = 0;
        user_config.left_handed = false;
        user_config.horizontal_scroll_arrows = false;
        user_config.vertical_scroll_arrows = false;
        user_config.drag_scroll_mode = false;
        user_config.left_knob_invert = false;
        user_config.right_knob_invert = false;
        user_config.led_brightness = 1;             // Dimmest visible level (0 = off).
        user_config.gesture_tap_keycode = MS_BTN7;
        user_config.drag_scroll_tap_keycode = MS_BTN8;
        eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));
    }

    /* Load scroll-accel params from the EECONFIG user-data block (set by the web
       UI via via_custom_value_command_kb). On a firmware upgrade the block may be
       all-zero / 0xFF (not NaN) yet never written, so also guard by valid range --
       otherwise mid/k/min/max could read 0 and kill scrolling. If invalid, reseed
       defaults and persist them. */
     eeconfig_read_user_datablock(&g_scroll_accel_config, SCROLL_ACCEL_EEPROM_OFFSET, sizeof(scroll_accel_config_t));
     if (isnan(g_scroll_accel_config.offset) || isnan(g_scroll_accel_config.takeoff) ||
         isnan(g_scroll_accel_config.growth_rate) || isnan(g_scroll_accel_config.limit) ||
         isnan(g_scroll_accel_config.limit_upper) ||
         g_scroll_accel_config.offset < SCROLL_ACCEL_R_MIN_OFFSET || g_scroll_accel_config.offset > SCROLL_ACCEL_R_MAX_OFFSET ||
         g_scroll_accel_config.takeoff < SCROLL_ACCEL_R_MIN_TAKEOFF || g_scroll_accel_config.takeoff > SCROLL_ACCEL_R_MAX_TAKEOFF ||
         g_scroll_accel_config.growth_rate < SCROLL_ACCEL_R_MIN_GROWTH_RATE || g_scroll_accel_config.growth_rate > SCROLL_ACCEL_R_MAX_GROWTH_RATE ||
         g_scroll_accel_config.limit < SCROLL_ACCEL_R_MIN_LIMIT || g_scroll_accel_config.limit > SCROLL_ACCEL_R_MAX_LIMIT ||
         g_scroll_accel_config.limit_upper < SCROLL_ACCEL_R_MIN_LIMIT_UPPER || g_scroll_accel_config.limit_upper > SCROLL_ACCEL_R_MAX_LIMIT_UPPER ||
         /* A freshly-erased block reads as all-zero; offset=0 is in-range so it would
            otherwise pass the checks above. Treat an all-zero block as uninitialized. */
         (g_scroll_accel_config.offset == 0.0f && g_scroll_accel_config.takeoff == 0.0f &&
          g_scroll_accel_config.growth_rate == 0.0f && g_scroll_accel_config.limit == 0.0f &&
          g_scroll_accel_config.limit_upper == 0.0f && g_scroll_accel_config.enabled == 0)) {
        g_scroll_accel_config = (scroll_accel_config_t){
            .enabled = 1,
            .offset = 220.0f,
            .takeoff = 0.03f,
            .growth_rate = 0.012f,
            .limit = 1.3f,
            .limit_upper = 5.0f,
        };
        scroll_accel_config_update();
    }
    scroll_accel_rebuild_lut(); // 开机初始化时，预先生成一次 Sigmoid 查表

    /* Load DPI presets/active preset from the EECONFIG user-data block. Reseed
       defaults if it was never written (all-zero) or is corrupt (active >= 5). */
    eeconfig_read_user_datablock(&g_dpi_config, DPI_EEPROM_OFFSET, sizeof(dpi_config_t));
    if (g_dpi_config.active_preset >= 5 ||
        (g_dpi_config.presets[0] == 0 && g_dpi_config.presets[1] == 0 &&
         g_dpi_config.presets[2] == 0 && g_dpi_config.presets[3] == 0 &&
         g_dpi_config.presets[4] == 0)) {
        g_dpi_config = (dpi_config_t){
            .presets = { 800, 1200, 1600, 2000, 2400 },
            .active_preset = 1,
        };
        dpi_config_update();
    }
    dpi_apply();

    /* Load the auto-sleep config (on/off + timeout in minutes) from the
       EECONFIG user-data block.
       Catch the ways the tail of the block can be invalid:
       - timeout out of range (all-zero invalid-datablock read, or erased
         0xFFFF) means the WHOLE struct is fresh/unread -> reseed both
         fields, so an uninitialized block never boots with sleep disabled;
       - timeout fine but the enabled byte > 1 (corrupt tail from a layout
         change) -> reseed only the enabled byte, keeping the user's timeout. */
    eeconfig_read_user_datablock(&g_sleep_config, SLEEP_EEPROM_OFFSET, sizeof(sleep_config_t));
    if (g_sleep_config.timeout_min < SLEEP_UI_MIN_MIN ||
        g_sleep_config.timeout_min > SLEEP_UI_MAX_MIN) {
        g_sleep_config = (sleep_config_t){
            .timeout_min = TRACKBALL_SLEEP_DEFAULT_TIMEOUT_MIN,
            .enabled     = SLEEP_DEFAULT_ENABLED,
        };
        sleep_config_update();
    } else if (g_sleep_config.enabled > 1) {
        g_sleep_config.enabled = SLEEP_DEFAULT_ENABLED;
        sleep_config_update();
    }

    /* Load the drag-scroll divisors (mouse x/y counts per scroll tick) from
       the EECONFIG user-data block. The 8-byte tail may be fresh on a layout
       growth (reads as 0x00, 0xFF -> NaN, or stale bytes): treat any NaN or
       out-of-range value as uninitialized and reseed the macro defaults so
       the accumulator can never divide by 0. */
    eeconfig_read_user_datablock(&g_drag_scroll_config, DRAGSCROLL_EEPROM_OFFSET, sizeof(drag_scroll_config_t));
    if (isnan(g_drag_scroll_config.h) || isnan(g_drag_scroll_config.v) ||
        g_drag_scroll_config.h < DRAGSCROLL_R_MIN || g_drag_scroll_config.h > DRAGSCROLL_R_MAX ||
        g_drag_scroll_config.v < DRAGSCROLL_R_MIN || g_drag_scroll_config.v > DRAGSCROLL_R_MAX ||
        (g_drag_scroll_config.h == 0.0f && g_drag_scroll_config.v == 0.0f)) {
        g_drag_scroll_config = (drag_scroll_config_t){
            .h = PLOOPY_DRAGSCROLL_DIVISOR_H,
            .v = PLOOPY_DRAGSCROLL_DIVISOR_V,
        };
        drag_scroll_config_update();
    }
    drag_scroll_apply();

    /* 加载自定义 LED 配色 */
    eeconfig_read_user_datablock(&g_led_config, LED_CONFIG_EEPROM_OFFSET, sizeof(led_config_t));
    if (g_led_config.righty_sat == 0 && g_led_config.lefty_sat == 0 &&
        g_led_config.righty_hue == 0 && g_led_config.lefty_hue == 0) {
        g_led_config = (led_config_t){
            .righty_hue = 152,
            .righty_sat = 255,
            .lefty_hue  = 85,
            .lefty_sat  = 255,
        };
        led_config_update();
    }
    led_config_apply();

    /* Set the startup layer based on handed-ness. */
    if( user_config.left_handed ) {
        layer_on(LAYER_NAV_LEFT_HANDED);
    }


    /* ============================ Lighting Setup ========================= */

    /* Enable the LED layers. */
    rgblight_layers = my_rgb_layers;

    /* Enable RGB lighting for layer indicators. */
    rgblight_enable_noeeprom();
    rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);

    /* Set an initial colour to establish a baseline brightness for the RGB layers. */
    rgblight_sethsv(0, 0, RGBLIGHT_VAL_STEP * user_config.led_brightness);
    if (user_config.led_brightness <= 0) {
        rgblight_disable_noeeprom();
    }

    /* Start the auto-sleep idle timer from now, so the board never boots
       already asleep. */
    s_trackball_last_activity = timer_read32();
}

bool process_record_user(uint16_t keycode, keyrecord_t* record) {
    /* Any key event (press or release) wakes the trackball from auto-sleep. */
    trackball_wake();

    switch (keycode) {
        case PKC_TGL_MIRROR:
            if (record->event.pressed) {
                /* Toggle handed-ness, record new state to EEPROM. */
                user_config.left_handed ^= 1;
                eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));

                /* Activate the correct navigation layer and blink the LEDs. */
                if( user_config.left_handed ) {
                    dprintf( "Lefty nav mode.\n");
                    layer_on(LAYER_NAV_LEFT_HANDED);
                    rgblight_blink_layer(OPTION_CHANGED_LAYER_COLOUR, OPTION_CHANGE_BLINK_TIMEOUT*2);
                }
                else {
                    dprintf( "Righty nav mode.\n");
                    layer_off(LAYER_NAV_LEFT_HANDED);
                    rgblight_blink_layer_repeat(OPTION_CHANGED_LAYER_COLOUR, OPTION_CHANGE_BLINK_TIMEOUT, 2);
                }
            }
            return true;
        case PKC_TGL_VERT_SCRL:
            if (record->event.pressed) {
                user_config.vertical_scroll_arrows ^= 1;
                eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));

                if( user_config.vertical_scroll_arrows ) {
                    rgblight_blink_layer(OPTION_CHANGED_LAYER_COLOUR, OPTION_CHANGE_BLINK_TIMEOUT*2);
                    dprintf("Vert. scroll = arrows.\n");
                }
                else {
                    rgblight_blink_layer_repeat(OPTION_CHANGED_LAYER_COLOUR, OPTION_CHANGE_BLINK_TIMEOUT, 2);
                    dprintf("Vert. scroll = scroll events.\n");
                }
            }
            return true;
        case PKC_TGL_HORIZ_SCRL:
            if (record->event.pressed) {
                user_config.horizontal_scroll_arrows ^= 1;
                eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));

                if( user_config.horizontal_scroll_arrows ) {
                    rgblight_blink_layer(OPTION_CHANGED_LAYER_COLOUR, OPTION_CHANGE_BLINK_TIMEOUT*2);
                    dprintf("Horiz. scroll = arrows.\n");
                }
                else {
                    rgblight_blink_layer_repeat(OPTION_CHANGED_LAYER_COLOUR, OPTION_CHANGE_BLINK_TIMEOUT, 2);
                    dprintf("Horiz. scroll = scroll events.\n");
                }
            }
            return true;
        case PKC_TGL_DRAG_SCRL:
            if (record->event.pressed) {
                user_config.drag_scroll_mode ^= 1;
                eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));

                if( user_config.drag_scroll_mode ) {
                    rgblight_blink_layer(OPTION_CHANGED_LAYER_COLOUR, OPTION_CHANGE_BLINK_TIMEOUT*2);
                    dprintf("Drag scroll: toggle to activate.\n");
                }
                else {
                    rgblight_blink_layer_repeat(OPTION_CHANGED_LAYER_COLOUR, OPTION_CHANGE_BLINK_TIMEOUT, 2);
                    dprintf("Drag scroll: hold to activate.\n");
                }
            }
            return true;
        case PKC_GESTURE:
            if (record->event.pressed) {
                /* Send correct gesture mode activation based on preference. */
                pointing_device_gestures_start();
                rgblight_set_layer_state(GESTURE_LAYER_COLOUR, true);
            }
            /* Switch is released. */
            else {
                pointing_device_gestures_end();
                rgblight_set_layer_state(GESTURE_LAYER_COLOUR, false);
            }
            return true;
        case PKC_DRAG_SCROLL:
            if( user_config.drag_scroll_mode ) {
                if (record->event.pressed) {
                    toggle_drag_scroll();
                }
            }
            /* Press and hold mode. */
            else {
                is_drag_scroll = record->event.pressed;
            }
            return true;
        case PKC_ADJUST_LED_BRIGHTNESS:
            /* Increase brightness in RGBLIGHT_VAL_STEP steps.
               Go to minimum brightness if the max brightness is hit. */
            if (record->event.pressed) {
                if( rgblight_get_val() >= RGBLIGHT_VAL_STEP * 4 ) {
                    rgblight_decrease_val_noeeprom();
                    rgblight_decrease_val_noeeprom();
                    rgblight_decrease_val_noeeprom();
                    rgblight_decrease_val_noeeprom();
                    rgblight_disable_noeeprom();
                    user_config.led_brightness = 0;
                    eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));
                }
                else {
                    rgblight_enable_noeeprom();
                    rgblight_increase_val_noeeprom();
                    user_config.led_brightness += 1;
                    eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));
                }

                dprintf("Adjusted LED brightness. Current brightness = %d/255\n", rgblight_get_val());
            }
            return true;
        case PKC_BLINKY_DPI_CONFIG:
            if (record->event.pressed) {
                cycle_dpi();

                dprintf("DPI changed. Current DPI = %d\n", dpi_array[keyboard_config.dpi_config]);
                rgblight_blink_layer_repeat(OPTION_CHANGED_LAYER_COLOUR, OPTION_CHANGE_BLINK_TIMEOUT, keyboard_config.dpi_config + 1);
            }
            return true;
        default:
            return true;
    }
}

/* LED feedback for the gesture mode triggered by the TD_CONTROL_GESTURE key.
   Mirrors the lighting used by PKC_GESTURE. */
void gesture_mode_led_on(void) {
    rgblight_set_layer_state(GESTURE_LAYER_COLOUR, true);
}
void gesture_mode_led_off(void) {
    rgblight_set_layer_state(GESTURE_LAYER_COLOUR, false);
}

/* State variables for the scroll wheels. */
int32_t leftwheel_scroll_acc = 0;   /* Bresenham residual for accelerated scroll. */
int32_t rightwheel_scroll_acc = 0;

int16_t leftwheel_arrow_scroll_tick = 0;
int16_t rightwheel_arrow_scroll_tick = 0;

#define ACCEL_LUT_SIZE 512 // 覆盖 speed 从 0 到 511：让 Sigmoid 平滑爬升到 limit_upper，
                           // 避免在 speed>=128 处直接悬崖式钳到上限（那样中速就被当成高速）

static int32_t s_accel_lut[ACCEL_LUT_SIZE];
static int32_t s_accel_maxf = 256;
static int32_t s_accel_minf = 256;
static bool    s_lut_dirty  = true; // 标记查表是否需要更新

/* 当开机或 VIA 修改了加速参数时，重新计算生成一次查表 (仅在调参时运行一次) */
void scroll_accel_rebuild_lut(void) {
    if (!g_scroll_accel_config.enabled) {
        s_accel_maxf = 256;
        s_accel_minf = 256;
        for (int i = 0; i < ACCEL_LUT_SIZE; i++) {
            s_accel_lut[i] = 256;
        }
        s_lut_dirty = false;
        return;
    }

    float takeoff = g_scroll_accel_config.takeoff;
    if (takeoff <= 0.0001f) takeoff = 0.0001f; // 防止除以零

    float ratio       = g_scroll_accel_config.growth_rate / takeoff;
    float limit_upper = g_scroll_accel_config.limit_upper;
    float limit_min   = g_scroll_accel_config.limit;
    float offset      = g_scroll_accel_config.offset;

    s_accel_maxf = (int32_t)(limit_upper * 256.0f);
    s_accel_minf = (int32_t)(limit_min * 256.0f);

    /* 一次性生成 0~127 速度区间的所有 Sigmoid 倍率 */
    for (int speed = 0; speed < ACCEL_LUT_SIZE; speed++) {
        float exponent = takeoff * ((float)speed - offset);

        // 限制指数范围，防止浮点溢出
        if (exponent > 80.0f) exponent = 80.0f;
        if (exponent < -80.0f) exponent = -80.0f;

        float f = limit_upper - (limit_upper - limit_min) / powf(1.0f + expf(exponent), ratio);
        int32_t factor = (int32_t)(f * 256.0f);

        if (factor > s_accel_maxf) factor = s_accel_maxf;
        if (factor < s_accel_minf) factor = s_accel_minf;

        s_accel_lut[speed] = factor;
    }

    s_lut_dirty = false;
}

/* 通知系统 VIA 参数已改变，下次滚动时重新生成查表 */
void scroll_accel_config_changed(void) {
    s_lut_dirty = true;
}

/* Scroll acceleration tuning (generalised sigmoid curve), VIA-configurable.
     f = limit_upper - (limit_upper - limit) / (1 + e^(takeoff*(speed - offset)))^(growth_rate/takeoff)
   Gentle (≈limit) below offset, steepest near offset, then approaching limit_upper.
   growth_rate/takeoff (g/k) sets how fast the top is reached. All five values plus
   the enabled switch are runtime-adjustable from the VIA web UI (via_custom_value_command_kb). */
/* Accumulate wheel movement and emit scroll with a continuous, speed-dependent
   multiplier (no step jumps). Accumulation lets slow scrolling still register
   (important for the Mac/low-res path where base_div is large), and clearing
   the residual on direction reversal keeps the two directions symmetric.
   The Bresenham residual is carried in *acc (fine-grained, so no coarse
   low-speed beat), giving a smooth average rate. */
// static int16_t scroll_accel_apply_slow(int32_t *acc, int16_t delta, int16_t base_div) {
//     /* Drop any leftover residual when the wheel reverses, so scrolling one
//        direction doesn't "unwind" residual motion from the other. */
//     if ((delta > 0 && *acc < 0) || (delta < 0 && *acc > 0)) {
//         *acc = 0;
//     }

//     int16_t speed = abs(delta);
//     int32_t factor, maxf, minf;
//     if (!g_scroll_accel_config.enabled) {
//         /* Curve acceleration disabled: plain 1:1 scroll at the base divisor. */
//         factor = 256;
//         maxf = minf = 256;
//     } else {
//         /* Generalised sigmoid acceleration with a sub-1 low-speed floor, matching the
//            drashna/pointing_device_accel form:
//               f = limit_upper - (limit_upper - limit) / (1 + e^(takeoff*(speed - offset)))^(growth_rate/takeoff)
//            growth_rate/takeoff (g/k) shapes how fast the curve climbs to limit_upper.
//            No kink; top reached at a finite speed. */
//         float ratio = g_scroll_accel_config.growth_rate / g_scroll_accel_config.takeoff;
//         float f = g_scroll_accel_config.limit_upper -
//                   (g_scroll_accel_config.limit_upper - g_scroll_accel_config.limit) /
//                       powf(1.0f + expf(g_scroll_accel_config.takeoff * (speed - g_scroll_accel_config.offset)), ratio);
//         factor = (int32_t)(f * 256.0f);
//         maxf = (int32_t)(g_scroll_accel_config.limit_upper * 256.0f);
//         minf = (int32_t)(g_scroll_accel_config.limit * 256.0f);
//     }
//     if (factor > maxf) factor = maxf;
//     if (factor < minf) factor = minf;

//     /* Bresenham: residual += motion * factor; emit integer scrolls of
//        (base_div * 256) units each. Keep the residual SIGNED in (-D, D) via
//        truncating division -- a sub-tick nudge the other way only shifts the
//        signed residual, never emits a phantom tick. (Flooring a negative partial
//        down to -1, as Euclidean division does, made the wheel emit +/-1 every
//        tick at rest: the continuous post-scroll jitter.) */
//     int32_t D = (int32_t)base_div * 256;
//     int32_t frac = *acc + (int32_t)delta * factor;
//     int32_t out32 = frac / D;
//     *acc = frac % D;
//     /* mouse_report.v / .h are int8_t; clamp to avoid HID wraparound on very fast
//        flicks (delta * factor can exceed 127 per tick). */
//     if (out32 > 127) {
//         out32 = 127;
//     } else if (out32 < -127) {
//         out32 = -127;
//     }
//     return (int16_t)out32;
// }

/* ===========================================================================
 * 极速版 Bresenham 滚动加速应用函数 (纯整数运算，零软浮点开销)
 * ========================================================================= */
static int16_t scroll_accel_apply(int32_t *acc, int16_t delta, int16_t base_div) {
    /* 反向旋转时立刻清空残差 */
    if ((delta > 0 && *acc < 0) || (delta < 0 && *acc > 0)) {
        *acc = 0;
    }

    /* 如果 VIA 刚修改了参数，懒加载重新生成查表 */
    if (s_lut_dirty) {
        scroll_accel_rebuild_lut();
    }

    /* 用有符号转无符号避免 abs(INT16_MIN) 的实现定义行为（虽 delta 已被
       calculate_wheel_delta 限制在 ±8191，仍做防御）。 */
    int32_t speed = (delta < 0) ? -(int32_t)delta : (int32_t)delta;
    int32_t factor;

    /* 【核心优化】：瞬间 O(1) 纯整数查表，耗时 8 纳秒，完全消除 powf/expf 开销！ */
    if (speed < ACCEL_LUT_SIZE) {
        factor = s_accel_lut[speed];
    } else {
        factor = s_accel_maxf; // 极端超速直接取上限
    }

    /* Bresenham 整数小数累加器 */
    int32_t D     = (int32_t)base_div * 256;
    int32_t frac  = *acc + (int32_t)delta * factor;
    int32_t out32 = frac / D;
    *acc          = frac % D;

    /* HID 报告范围钳位 [-127, 127] */
    if (out32 > 127) {
        out32 = 127;
    } else if (out32 < -127) {
        out32 = -127;
    }
    return (int16_t)out32;
}

int16_t leftwheel_volume_scroll_tick = 0;
int16_t rightwheel_volume_scroll_tick = 0;

/* Low-pass filter state for wheel deltas (smooths quantisation noise at slow
   scroll speeds). α = 0.3 → time-constant ≈ 2 polls ≈ 30 ms at 15 ms rate. */
static int16_t leftwheel_filtered_delta  = 0;
static int16_t rightwheel_filtered_delta = 0;
#define WHEEL_DELTA_FILTER_ALPHA_NUM 3
#define WHEEL_DELTA_FILTER_ALPHA_DEN 10
/* The delta low-pass filter keeps its state scaled up by this much, so the
   integer division cannot swallow small deltas. With the plain integer EMA a
   steady low-speed delta of 1-3 units collapsed to 0 (ALL motion lost) and
   4-10 units lost 30-75%, while high speed only lost ~6-10% -- which is why
   slow scrolling stuttered or did not move at all. Scaling first keeps the
   loss under ~1% at every speed. */
#define WHEEL_DELTA_FILTER_SCALE 256
static int32_t leftwheel_filtered_delta_state  = 0;
static int32_t rightwheel_filtered_delta_state = 0;

/* Below this magnitude the filtered per-poll delta is treated as sensor /
   mechanical noise and forced to zero. 2 angle LSB is 0.125 degrees; the
   TMAG5273 angle quantisation/noise floor plus the wheel's tiny settling
   wobble is in that range. This matters especially on Windows: the hi-res
   path uses a divisor of 2, so a single leftover LSB would otherwise feed
   the Bresenham accumulator and eventually emit a 1/120-notch scroll event.
   The old per-poll integer division by 2 discarded these values; the
   accumulator did not, which is why random micro-scrolls appeared. Real
   slow scrolling settles above this gate once the low-pass filter ramps. */
#ifndef WHEEL_DELTA_NOISE_GATE
#    define WHEEL_DELTA_NOISE_GATE 2
#endif

uint32_t leftwheel_timeout = 0;
uint32_t rightwheel_timeout = 0;

static float mac_drag_scroll_x_accum = 0.0f;
static float mac_drag_scroll_y_accum = 0.0f;

report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {

    /* Reduce drag-scroll speed on macOS only. */
    if (is_drag_scroll && detected_host_os() == OS_MACOS) {
        mac_drag_scroll_x_accum += (float)mouse_report.h / 48.0f;
        mac_drag_scroll_y_accum += (float)mouse_report.v / 48.0f;

        mouse_report.h = (int8_t)mac_drag_scroll_x_accum;
        mouse_report.v = (int8_t)mac_drag_scroll_y_accum;

        mac_drag_scroll_x_accum -= (int8_t)mac_drag_scroll_x_accum;
        mac_drag_scroll_y_accum -= (int8_t)mac_drag_scroll_y_accum;
    } else {
        mac_drag_scroll_x_accum = 0.0f;
        mac_drag_scroll_y_accum = 0.0f;
    }

    /* While awake, any ball or button motion holds off auto-sleep by pushing
       the idle timer forward. This only refreshes the timer; it cannot wake
       from sleep because the hardware is truly off then -- only a key press can
       do that. */
    if (mouse_report.x != 0 || mouse_report.y != 0 || mouse_report.buttons != 0) {
        s_trackball_last_activity = timer_read32();
    }

    /* While asleep, suppress all pointing output and return before the 20 ms
       block below, so the two wheels are not polled at all. v/h must be zeroed
       as well: the scroll code only ever writes them, so a stale value would
       otherwise leak through. */
    if (s_trackball_asleep) {
        mouse_report.x = 0;
        mouse_report.y = 0;
        mouse_report.v = 0;
        mouse_report.h = 0;
        return mouse_report;
    }

    // throttle reads
    if (timer_elapsed32(last_scroll_time) > 20) {
        uint16_t leftwheel_rawangle = tmag5273_get_angle(TMAG5273_D0_I2C_ADDRESS);
        uint16_t rightwheel_rawangle = tmag5273_get_angle(TMAG5273_D1_I2C_ADDRESS);

        /* Default to hi-res: the HID descriptor advertises the resolution
           multiplier, so 1 notch = POINTING_DEVICE_HIRES_SCROLL_MULTIPLIER
           (120) units on every host that honours it (Windows, Linux, and
           anything unknown). Only a *positive* macOS detection falls back to
           coarse ticks, because macOS ignores the multiplier and treats each
           unit as one detent (the hi-res divisor would then scroll hundreds
           of lines per revolution). Anything not identified as macOS --
           including OS_UNSURE -- keeps the hi-res divisors. */
        bool hi_res = (detected_host_os() != OS_MACOS);

        /* First poll after waking: the stored wheel positions are stale because
           we were not polling, so re-baseline them against where the wheels are
           now (discarding anything that moved while asleep) and drop this
           tick's wheel input rather than emitting one huge burst of scroll. */
        if (s_wheel_resync_pending) {
            s_wheel_resync_pending        = false;
            leftwheel_current_position    = leftwheel_rawangle;
            leftwheel_deadzone_center     = leftwheel_rawangle;
            rightwheel_current_position   = rightwheel_rawangle;
            rightwheel_deadzone_center    = rightwheel_rawangle;
            leftwheel_filtered_delta_state  = 0;
            rightwheel_filtered_delta_state = 0;
            leftwheel_filtered_delta        = 0;
            rightwheel_filtered_delta       = 0;
            leftwheel_scroll_acc            = 0;
            rightwheel_scroll_acc           = 0;
            last_scroll_time = timer_read32();
            return mouse_report;
        }

        int16_t leftwheel_deadzone_distance = calculate_deadzone_distance( leftwheel_rawangle, &leftwheel_deadzone_center );
        int16_t rightwheel_deadzone_distance = calculate_deadzone_distance( rightwheel_rawangle, &rightwheel_deadzone_center );

        int16_t leftwheel_delta = calculate_wheel_delta( leftwheel_rawangle, leftwheel_current_position );
        leftwheel_current_position = leftwheel_rawangle;

        int16_t rightwheel_delta = calculate_wheel_delta( rightwheel_rawangle, rightwheel_current_position );
        rightwheel_current_position = rightwheel_rawangle;

        /* Low-pass filter the deltas to smooth quantisation noise at slow
           scroll speeds. The gated filtered delta is also what the idle-timeout
           check below uses. */
        leftwheel_filtered_delta_state  = (WHEEL_DELTA_FILTER_ALPHA_NUM * (int32_t)leftwheel_delta  * WHEEL_DELTA_FILTER_SCALE +
                                          (WHEEL_DELTA_FILTER_ALPHA_DEN - WHEEL_DELTA_FILTER_ALPHA_NUM) * leftwheel_filtered_delta_state)  / WHEEL_DELTA_FILTER_ALPHA_DEN;
        rightwheel_filtered_delta_state = (WHEEL_DELTA_FILTER_ALPHA_NUM * (int32_t)rightwheel_delta * WHEEL_DELTA_FILTER_SCALE +
                                          (WHEEL_DELTA_FILTER_ALPHA_DEN - WHEEL_DELTA_FILTER_ALPHA_NUM) * rightwheel_filtered_delta_state) / WHEEL_DELTA_FILTER_ALPHA_DEN;
        leftwheel_filtered_delta  = (int16_t)(leftwheel_filtered_delta_state  / WHEEL_DELTA_FILTER_SCALE);
        rightwheel_filtered_delta = (int16_t)(rightwheel_filtered_delta_state / WHEEL_DELTA_FILTER_SCALE);

        /* Kill sub-LSB noise before it reaches any consumer (scroll, volume,
           arrow modes). Without this the accumulator turns idle sensor jitter
           into occasional +/-1 hires scroll counts, which Windows renders as
           visible random small up/down scrolling. Only needed on hi-res hosts;
           see the hi_res comment above. */
        if (hi_res) {
            if (abs(leftwheel_filtered_delta) < WHEEL_DELTA_NOISE_GATE) {
                leftwheel_filtered_delta = 0;
            }
            if (abs(rightwheel_filtered_delta) < WHEEL_DELTA_NOISE_GATE) {
                rightwheel_filtered_delta = 0;
            }
        }

        /* Per-knob direction inversion (VIA "Knobs", default off).
           Applied here -- on the filtered delta, the single source every
           consumer reads (normal scroll, arrow modes, Control-layer volume) --
           so flipping it flips that knob's output consistently. The raw delta
           and deadzone distance are deliberately untouched: they gate the
           deadzone / timeout / idle checks, which use abs() and are
           sign-independent. */
        if (user_config.left_knob_invert) {
            leftwheel_filtered_delta = -leftwheel_filtered_delta;
        }
        if (user_config.right_knob_invert) {
            rightwheel_filtered_delta = -rightwheel_filtered_delta;
        }

        /* Wheel/scroll motion holds off auto-sleep, but ONLY on deliberate
           scrolling -- i.e. movement past the deadzone -- NOT on the few-LSB
           angle jitter the TMAG reports while at rest. Gating on the raw delta
           (> 2) would trip on that jitter every poll and perpetually reset the
           idle timer, so the board would never reach the sleep timeout. */
        if (abs(leftwheel_deadzone_distance) > TMAG5273_WHEEL_DEADZONE ||
            abs(rightwheel_deadzone_distance) > TMAG5273_WHEEL_DEADZONE) {
            s_trackball_last_activity = timer_read32();
        }

        /* If either wheel moved, we reset the timeout counter. Else, we start
           counting inactivity. Use the gated filtered delta, not the raw one:
           raw sensor jitter above 2 LSB used to reset this timeout forever, so
           the deadzone center never re-latched and the gate stayed open. */
        if( leftwheel_filtered_delta != 0 ) {
            leftwheel_timeout = 0;
        }
        else {
            /* Don't increment forever, you'll get rollover. */
            if( leftwheel_timeout < TMAG5273_DEADZONE_PROTECTOR_TIMEOUT ) {
                leftwheel_timeout += timer_elapsed32(last_scroll_time);
            }
        }
        if( rightwheel_filtered_delta != 0 ) {
            rightwheel_timeout = 0;
        }
        else {
            /* Don't increment forever, you'll get rollover. */
            if( rightwheel_timeout < TMAG5273_DEADZONE_PROTECTOR_TIMEOUT ) {
                rightwheel_timeout += timer_elapsed32(last_scroll_time);
            }
        }

        /* Reset the counter so we know when to next perform all this scroll processing. */
        last_scroll_time = timer_read32();

        /* If either of the wheels times out, we move the deadzone window to
           where the position is. This prevents spurious scroll events. */
        if( leftwheel_timeout >= TMAG5273_DEADZONE_PROTECTOR_TIMEOUT &&
            leftwheel_timeout < TMAG5273_DEADZONE_PROTECTOR_TIMEOUT * 2 ) {

            leftwheel_deadzone_center = leftwheel_current_position;
            dprintf("Left wheel timeout, deadzone moved to: %d\n", leftwheel_deadzone_center);

            /* Set the timeout to a large number so that we only shift the window once. */
            leftwheel_timeout = TMAG5273_DEADZONE_PROTECTOR_TIMEOUT * 4;
        }
        if( rightwheel_timeout >= TMAG5273_DEADZONE_PROTECTOR_TIMEOUT &&
            rightwheel_timeout < TMAG5273_DEADZONE_PROTECTOR_TIMEOUT * 2 ) {

            rightwheel_deadzone_center = rightwheel_current_position;
            dprintf("Right wheel timeout, deadzone moved to: %d\n", rightwheel_deadzone_center);

            /* Set the timeout to a large number so that we only shift the window once. */
            rightwheel_timeout = TMAG5273_DEADZONE_PROTECTOR_TIMEOUT * 4;
        }

        /* If we're on the control layers, both wheels adjust the volume. */
        if( layer_state_is(LAYER_CONTROL) ) {

            leftwheel_volume_scroll_tick += leftwheel_filtered_delta;
            rightwheel_volume_scroll_tick += rightwheel_filtered_delta;

            if( leftwheel_volume_scroll_tick > TMAG5273_VOLUME_SCROLL_TICK_SIZE ) {
                tap_code(KC_VOLU);
                leftwheel_volume_scroll_tick = 0;
            }
            else if( leftwheel_volume_scroll_tick < -TMAG5273_VOLUME_SCROLL_TICK_SIZE ) {
                tap_code(KC_VOLD);
                leftwheel_volume_scroll_tick = 0;
            }
            if( rightwheel_volume_scroll_tick > TMAG5273_VOLUME_SCROLL_TICK_SIZE ) {
                tap_code(KC_VOLU);
                rightwheel_volume_scroll_tick = 0;
            }
            else if( rightwheel_volume_scroll_tick < -TMAG5273_VOLUME_SCROLL_TICK_SIZE ) {
                tap_code(KC_VOLD);
                rightwheel_volume_scroll_tick = 0;
            }

            /* Return early -- no other scrolling activity happens in the
               control layer. */
            return mouse_report;
        }

        /* If scroll arrow mode is activated for horizontal scrolling, then we do that.
           This behaviour is the same across OSes. */
        if( user_config.horizontal_scroll_arrows ) {

            leftwheel_arrow_scroll_tick += leftwheel_filtered_delta;
            rightwheel_arrow_scroll_tick += rightwheel_filtered_delta;

            if( user_config.left_handed ) {
                if( leftwheel_arrow_scroll_tick > TMAG5273_HORIZ_SCROLL_TICK_SIZE ) {
                    tap_code(KC_RIGHT);
                    leftwheel_arrow_scroll_tick = 0;
                }
                else if( leftwheel_arrow_scroll_tick < -TMAG5273_HORIZ_SCROLL_TICK_SIZE ) {
                    tap_code(KC_LEFT);
                    leftwheel_arrow_scroll_tick = 0;
                }

                /* Set the delta to zero so we don't scroll *and* arrow at the same time. */
                leftwheel_delta = 0;
            }
            else {
                if( rightwheel_arrow_scroll_tick > TMAG5273_HORIZ_SCROLL_TICK_SIZE ) {
                    tap_code(KC_RIGHT);
                    rightwheel_arrow_scroll_tick = 0;
                }
                else if( rightwheel_arrow_scroll_tick < -TMAG5273_HORIZ_SCROLL_TICK_SIZE ) {
                    tap_code(KC_LEFT);
                    rightwheel_arrow_scroll_tick = 0;
                }
                /* Set the delta to zero so we don't scroll *and* arrow at the same time. */
                rightwheel_delta = 0;
            }
        }

        /* If scroll arrow mode is activated for vertical scrolling, then we do that.
           This behaviour is the same across OSes. */
        if( user_config.vertical_scroll_arrows ) {

            leftwheel_arrow_scroll_tick += leftwheel_filtered_delta;
            rightwheel_arrow_scroll_tick += rightwheel_filtered_delta;

            if( user_config.left_handed ) {
                if( rightwheel_arrow_scroll_tick > TMAG5273_VERT_SCROLL_TICK_SIZE ) {
                    tap_code(KC_DOWN);
                    rightwheel_arrow_scroll_tick = 0;
                }
                else if( rightwheel_arrow_scroll_tick < -TMAG5273_VERT_SCROLL_TICK_SIZE ) {
                    tap_code(KC_UP);
                    rightwheel_arrow_scroll_tick = 0;
                }
                /* Set the delta to zero so we don't scroll *and* arrow at the same time. */
                rightwheel_delta = 0;
            }
            else {
                if( leftwheel_arrow_scroll_tick > TMAG5273_VERT_SCROLL_TICK_SIZE ) {
                    tap_code(KC_DOWN);
                    leftwheel_arrow_scroll_tick = 0;
                }
                else if( leftwheel_arrow_scroll_tick < -TMAG5273_VERT_SCROLL_TICK_SIZE ) {
                    tap_code(KC_UP);
                    leftwheel_arrow_scroll_tick = 0;
                }
                /* Set the delta to zero so we don't scroll *and* arrow at the same time. */
                leftwheel_delta = 0;
            }
        }

        /* Accumulate (and emit) only once the wheel is OUTSIDE the deadzone.
           Inside the deadzone scroll_accel_apply() is simply not called, so the
           Bresenham residual is left untouched (it is NOT cleared) and slow
           motion keeps accumulating towards the next emitted tick. The
           wheel->axis mapping is identical for hi-res and low-res OSes; only
           the divisor differs, so the deadzone gate applies uniformly
           (incl. macOS). */
        bool left_is_h  = user_config.left_handed;
        bool right_is_h = !user_config.left_handed;

        int16_t left_div  = hi_res ? (left_is_h  ? TMAG5273_HORIZONAL_WHEEL_SPEED_DIV : TMAG5273_VERTICAL_WHEEL_SPEED_DIV)
                                   : TMAG5273_LOWRES_TICK_SIZE;
        int16_t right_div = hi_res ? (right_is_h ? TMAG5273_HORIZONAL_WHEEL_SPEED_DIV : TMAG5273_VERTICAL_WHEEL_SPEED_DIV)
                                   : TMAG5273_LOWRES_TICK_SIZE;

        bool left_outside  = leftwheel_deadzone_distance  > (TMAG5273_WHEEL_DEADZONE - 10) ||
                             leftwheel_deadzone_distance  < (-TMAG5273_WHEEL_DEADZONE + 10);
        bool right_outside = rightwheel_deadzone_distance > (TMAG5273_WHEEL_DEADZONE - 10) ||
                             rightwheel_deadzone_distance < (-TMAG5273_WHEEL_DEADZONE + 10);

        if ( left_outside ) {
            if ( left_is_h ) {
                if ( !user_config.horizontal_scroll_arrows ) {
                    mouse_report.h = scroll_accel_apply(&leftwheel_scroll_acc, leftwheel_filtered_delta, left_div);
                }
            } else {
                mouse_report.v = -scroll_accel_apply(&leftwheel_scroll_acc, leftwheel_filtered_delta, left_div);
            }
        }

        if ( right_outside ) {
            if ( right_is_h ) {
                if ( !user_config.horizontal_scroll_arrows ) {
                    mouse_report.h = scroll_accel_apply(&rightwheel_scroll_acc, rightwheel_filtered_delta, right_div);
                }
            } else {
                mouse_report.v = -scroll_accel_apply(&rightwheel_scroll_acc, rightwheel_filtered_delta, right_div);
            }
        }

        /* Set scroll data to zero if the corresponding button is
           currently being held down. */

        /* Check left knob. */
        if( matrix_is_on(0,6) ) {
            if( user_config.left_handed ) {
                mouse_report.h = 0;
            }
            else {
                mouse_report.v = 0;
            }
        }
        /* Check right knob. */
        if( matrix_is_on(0,7) ) {
            if( user_config.left_handed ) {
                mouse_report.v = 0;
            }
            else {
                mouse_report.h = 0;
            }
        }

    }

    return mouse_report;
}

/* ===================================================================== *
 *  VIA custom-value menu (channel 25).
 *  Scroll-accel params (mid, k, min, max_boost; value ids 1..4) are edited
 *  live from the VIA web UI and persisted in VIA's custom EEPROM region.
 *  The gesture tap keycode (value id 5) is edited live too and persisted in
 *  the regular user_config EEPROM. The web menu is aplus.json "menus".
 * ===================================================================== */

enum via_scroll_accel_channel {
    id_scroll_accel = 25,
};

enum via_scroll_accel_ids {
    id_scroll_accel_offset = 1,
    id_scroll_accel_takeoff = 2,
    id_scroll_accel_limit = 3,
    id_scroll_accel_limit_upper = 4,
    id_scroll_accel_gesture_tap_keycode = 5,
    id_scroll_accel_enabled = 6,
    id_scroll_accel_growth_rate = 7,
    id_scroll_accel_drag_scroll_tap_keycode = 8,
    id_scroll_accel_drag_divisor_h = 9,
    id_scroll_accel_drag_divisor_v = 10,
    id_scroll_accel_knob_invert_left = 11,
    id_scroll_accel_knob_invert_right = 12,
};

/* Separate VIA channel for DPI so it never mixes with the scroll UI. The web
   UI (aplus.json "menus") drives it with channel 26 and the value ids below. */
enum via_dpi_channel {
    id_dpi = 26,
};

enum via_dpi_ids {
    id_dpi_activepreset = 1,
    id_dpi_preset0 = 2,
    id_dpi_preset1 = 3,
    id_dpi_preset2 = 4,
    id_dpi_preset3 = 5,
    id_dpi_preset4 = 6,
};

/* Separate VIA channel for the auto-sleep timeout so it never mixes with the
   scroll or DPI UI. The web menu (aplus.json "menus" -> Settings -> Power)
   drives it with channel 27 and the value ids below. */
enum via_sleep_channel {
    id_sleep = 27,
};

enum via_sleep_ids {
    id_sleep_timeout_min = 1,
    id_sleep_enabled = 2,
};

/* Separate VIA channel for the sensor diagnostics. The web menu
   (aplus.json "menus" -> Settings -> Sensor) drives it with channel 28. */
enum via_sensor_channel {
    id_sensor = 28,
};

enum via_sensor_ids {
    id_sensor_squal    = 1,
    id_sensor_shutter  = 2,
    id_sensor_sampling = 3,
    id_sensor_max      = 4,
    id_sensor_min      = 5,
    id_sensor_pixsum   = 7,
    id_sensor_quality  = 8,
};

/* Sampling state (s_sensor_sampling and the sum/count accumulators) is
   declared with the other statics near trackball_sleep_now(). */

/* DPI presets exposed to the web UI as real CPI values. The active preset
   index is 0..4; the five preset controls are plain uint16 ranges. */
#define DPI_UI_MIN_PRESET 100
#define DPI_UI_MAX_PRESET 3200

/* VIA sliders are uint16. To keep the web UI readable, each float param is
   exposed in its natural units (scaled to an integer). The aplus.json "options"
   ranges must match these integer ranges. The stored config stays in real float
   units, so the per-param real ranges (SCROLL_ACCEL_R_*) used by the init guard
   in keyboard_post_init_user are unchanged.
      offset      : speed units,     scale x1    (UI 0..400)
      takeoff     : steepness,       scale x1000 (UI 5..100    -> 0.005..0.1)
      growth_rate : top saturation,  scale x1000 (UI 1..100    -> 0.001..0.1)
      limit       : low-speed floor, scale x1000 (UI 500..1000 -> 0.5..1.0)
      limit_upper : top multiplier,  scale x1    (UI 1..10) */
#define SCROLL_ACCEL_UI_OFFSET_SCALE       1
#define SCROLL_ACCEL_UI_TAKEOFF_SCALE      1000
#define SCROLL_ACCEL_UI_GROWTH_RATE_SCALE  1000
#define SCROLL_ACCEL_UI_LIMIT_SCALE        1000
#define SCROLL_ACCEL_UI_LIMIT_UPPER_SCALE  1

/* VIA encodes a `range` control as 2 big-endian bytes when its options max > 255,
   otherwise as a single byte. Our controls: offset (max 400) and limit (max 1000) use
   2-byte BE; takeoff (max 100), growth_rate (max 100) and limit_upper (max 10) use 1-byte;
   enabled is a 1-byte toggle; gesture is a 2-byte
   BE keycode. The get path mirrors this so the web UI reads values back correctly. */
static void scroll_accel_config_set_value(uint8_t *data) {
    uint8_t *value_id = &(data[0]);
    uint8_t *value_data = &(data[1]);

    switch (*value_id) {
        case id_scroll_accel_offset: {
            uint16_t v = ((uint16_t)value_data[0] << 8) | value_data[1];
            g_scroll_accel_config.offset = (float)v / SCROLL_ACCEL_UI_OFFSET_SCALE;
            break;
        }
        case id_scroll_accel_takeoff: {
            uint16_t v = value_data[0];
            g_scroll_accel_config.takeoff = (float)v / SCROLL_ACCEL_UI_TAKEOFF_SCALE;
            break;
        }
        case id_scroll_accel_limit: {
            uint16_t v = ((uint16_t)value_data[0] << 8) | value_data[1];
            g_scroll_accel_config.limit = (float)v / SCROLL_ACCEL_UI_LIMIT_SCALE;
            break;
        }
        case id_scroll_accel_limit_upper: {
            uint16_t v = value_data[0];
            g_scroll_accel_config.limit_upper = (float)v / SCROLL_ACCEL_UI_LIMIT_UPPER_SCALE;
            break;
        }
        case id_scroll_accel_growth_rate: {
            uint16_t v = value_data[0];
            g_scroll_accel_config.growth_rate = (float)v / SCROLL_ACCEL_UI_GROWTH_RATE_SCALE;
            break;
        }
        case id_scroll_accel_enabled: {
            g_scroll_accel_config.enabled = value_data[0] ? 1 : 0;
            break;
        }
        case id_scroll_accel_gesture_tap_keycode: {
            uint16_t kc = ((uint16_t)value_data[0] << 8) | value_data[1];
            user_config.gesture_tap_keycode = kc;
            break;
        }
        case id_scroll_accel_drag_scroll_tap_keycode: {
            uint16_t kc = ((uint16_t)value_data[0] << 8) | value_data[1];
            user_config.drag_scroll_tap_keycode = kc;
            break;
        }
        case id_scroll_accel_drag_divisor_h: {
            uint16_t v = ((uint16_t)value_data[0] << 8) | value_data[1];
            if (v < (uint16_t)(DRAGSCROLL_R_MIN * DRAGSCROLL_UI_SCALE)) v = (uint16_t)(DRAGSCROLL_R_MIN * DRAGSCROLL_UI_SCALE);
            if (v > (uint16_t)(DRAGSCROLL_R_MAX * DRAGSCROLL_UI_SCALE)) v = (uint16_t)(DRAGSCROLL_R_MAX * DRAGSCROLL_UI_SCALE);
            g_drag_scroll_config.h = (float)v / DRAGSCROLL_UI_SCALE;
            drag_scroll_apply();
            break;
        }
        case id_scroll_accel_drag_divisor_v: {
            uint16_t v = ((uint16_t)value_data[0] << 8) | value_data[1];
            if (v < (uint16_t)(DRAGSCROLL_R_MIN * DRAGSCROLL_UI_SCALE)) v = (uint16_t)(DRAGSCROLL_R_MIN * DRAGSCROLL_UI_SCALE);
            if (v > (uint16_t)(DRAGSCROLL_R_MAX * DRAGSCROLL_UI_SCALE)) v = (uint16_t)(DRAGSCROLL_R_MAX * DRAGSCROLL_UI_SCALE);
            g_drag_scroll_config.v = (float)v / DRAGSCROLL_UI_SCALE;
            drag_scroll_apply();
            break;
        }
        case id_scroll_accel_knob_invert_left:
            user_config.left_knob_invert = value_data[0] ? true : false;
            break;
        case id_scroll_accel_knob_invert_right:
            user_config.right_knob_invert = value_data[0] ? true : false;
            break;
    }

    /* Persist immediately so a value change survives a reboot even if the host
       does not follow up with an explicit id_custom_save. */
    scroll_accel_config_update();
    eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));
    drag_scroll_config_update();
}

static void scroll_accel_config_get_value(uint8_t *data) {
    uint8_t *value_id = &(data[0]);
    uint8_t *value_data = &(data[1]);

    switch (*value_id) {
        case id_scroll_accel_offset: {
            uint16_t v = (uint16_t)(g_scroll_accel_config.offset * SCROLL_ACCEL_UI_OFFSET_SCALE);
            value_data[0] = v >> 8;
            value_data[1] = v & 0xFF;
            break;
        }
        case id_scroll_accel_takeoff: {
            uint16_t v = (uint16_t)(g_scroll_accel_config.takeoff * SCROLL_ACCEL_UI_TAKEOFF_SCALE);
            value_data[0] = v & 0xFF;
            value_data[1] = v >> 8;
            break;
        }
        case id_scroll_accel_limit: {
            uint16_t v = (uint16_t)(g_scroll_accel_config.limit * SCROLL_ACCEL_UI_LIMIT_SCALE);
            value_data[0] = v >> 8;
            value_data[1] = v & 0xFF;
            break;
        }
        case id_scroll_accel_limit_upper: {
            uint16_t v = (uint16_t)(g_scroll_accel_config.limit_upper * SCROLL_ACCEL_UI_LIMIT_UPPER_SCALE);
            value_data[0] = v & 0xFF;
            value_data[1] = v >> 8;
            break;
        }
        case id_scroll_accel_growth_rate: {
            uint16_t v = (uint16_t)(g_scroll_accel_config.growth_rate * SCROLL_ACCEL_UI_GROWTH_RATE_SCALE);
            value_data[0] = v & 0xFF;
            value_data[1] = v >> 8;
            break;
        }
        case id_scroll_accel_enabled: {
            value_data[0] = g_scroll_accel_config.enabled ? 1 : 0;
            value_data[1] = 0;
            break;
        }
        case id_scroll_accel_gesture_tap_keycode: {
            uint16_t kc = user_config.gesture_tap_keycode;
            value_data[0] = kc >> 8;
            value_data[1] = kc & 0xFF;
            break;
        }
        case id_scroll_accel_drag_scroll_tap_keycode: {
            uint16_t kc = user_config.drag_scroll_tap_keycode;
            value_data[0] = kc >> 8;
            value_data[1] = kc & 0xFF;
            break;
        }
        case id_scroll_accel_drag_divisor_h: {
            /* 2-byte BE, mirroring the set path (options max 2000 > 255). */
            uint16_t v = (uint16_t)(g_drag_scroll_config.h * DRAGSCROLL_UI_SCALE);
            value_data[0] = v >> 8;
            value_data[1] = v & 0xFF;
            break;
        }
        case id_scroll_accel_drag_divisor_v: {
            uint16_t v = (uint16_t)(g_drag_scroll_config.v * DRAGSCROLL_UI_SCALE);
            value_data[0] = v >> 8;
            value_data[1] = v & 0xFF;
            break;
        }
        case id_scroll_accel_knob_invert_left:
            value_data[0] = user_config.left_knob_invert ? 1 : 0;
            value_data[1] = 0;
            break;
        case id_scroll_accel_knob_invert_right:
            value_data[0] = user_config.right_knob_invert ? 1 : 0;
            value_data[1] = 0;
            break;
    }
}

static void scroll_accel_config_update(void) {
    eeconfig_update_user_datablock(&g_scroll_accel_config, SCROLL_ACCEL_EEPROM_OFFSET, sizeof(scroll_accel_config_t));
}

static void drag_scroll_config_update(void) {
    eeconfig_update_user_datablock(&g_drag_scroll_config, DRAGSCROLL_EEPROM_OFFSET, sizeof(drag_scroll_config_t));
}

/* Push the EEPROM copy into the runtime divisors ploopyco.c's drag-scroll
   accumulator divides by. Called at init and whenever a VIA slider moves. */
static void drag_scroll_apply(void) {
    drag_scroll_divisor_h = g_drag_scroll_config.h;
    drag_scroll_divisor_v = g_drag_scroll_config.v;
}

/* 将配置应用到 RAM 中的灯光段，并立即刷新物理灯光 */
static void led_config_apply(void) {
    righty_nav_layer_colour[0].hue = g_led_config.righty_hue;
    righty_nav_layer_colour[0].sat = g_led_config.righty_sat;
    lefty_nav_layer_colour[0].hue  = g_led_config.lefty_hue;
    lefty_nav_layer_colour[0].sat  = g_led_config.lefty_sat;

    /* 立即重新激活当前最高层的灯光，实现拖动滑块秒变色 */
    layer_state_set_user(layer_state);
}

static void led_config_update(void) {
    eeconfig_update_user_datablock(&g_led_config, LED_CONFIG_EEPROM_OFFSET, sizeof(led_config_t));
}

enum via_led_channel {
    id_led = 29,
};

enum via_led_ids {
    id_led_righty_hue = 1,
    id_led_righty_sat = 2,
    id_led_lefty_hue  = 3,
    id_led_lefty_sat  = 4,
};

static void led_config_set_value(uint8_t *data) {
    uint8_t *value_id = &(data[0]);
    uint8_t *value_data = &(data[1]);

    switch (*value_id) {
        case id_led_righty_hue:
            g_led_config.righty_hue = value_data[0];
            break;
        case id_led_righty_sat:
            g_led_config.righty_sat = value_data[0];
            break;
        case id_led_lefty_hue:
            g_led_config.lefty_hue = value_data[0];
            break;
        case id_led_lefty_sat:
            g_led_config.lefty_sat = value_data[0];
            break;
    }

    led_config_apply();
    led_config_update();
}

static void led_config_get_value(uint8_t *data) {
    uint8_t *value_id = &(data[0]);
    uint8_t *value_data = &(data[1]);

    switch (*value_id) {
        case id_led_righty_hue:
            value_data[0] = g_led_config.righty_hue;
            value_data[1] = 0;
            break;
        case id_led_righty_sat:
            value_data[0] = g_led_config.righty_sat;
            value_data[1] = 0;
            break;
        case id_led_lefty_hue:
            value_data[0] = g_led_config.lefty_hue;
            value_data[1] = 0;
            break;
        case id_led_lefty_sat:
            value_data[0] = g_led_config.lefty_sat;
            value_data[1] = 0;
            break;
    }
}

static void dpi_apply(void) {
    if (g_dpi_config.active_preset >= 5) g_dpi_config.active_preset = 0;
    pointing_device_set_cpi(g_dpi_config.presets[g_dpi_config.active_preset]);
}

static void dpi_config_update(void) {
    eeconfig_update_user_datablock(&g_dpi_config, DPI_EEPROM_OFFSET, sizeof(dpi_config_t));
}

static void dpi_config_set_value(uint8_t *data) {
    uint8_t *value_id = &(data[0]);
    uint8_t *value_data = &(data[1]);

    switch (*value_id) {
        case id_dpi_activepreset: {
            g_dpi_config.active_preset = (value_data[0] < 5) ? value_data[0] : 4;
            dpi_apply();
            break;
        }
        case id_dpi_preset0:
        case id_dpi_preset1:
        case id_dpi_preset2:
        case id_dpi_preset3:
        case id_dpi_preset4: {
            uint8_t idx = (uint8_t)(*value_id - id_dpi_preset0);
            uint16_t v = ((uint16_t)value_data[0] << 8) | value_data[1];
            g_dpi_config.presets[idx] = v;
            dpi_apply();
            break;
        }
    }

    /* Persist immediately so a value change survives a reboot even if the host
       does not follow up with an explicit id_custom_save. */
    dpi_config_update();
}

static void dpi_config_get_value(uint8_t *data) {
    uint8_t *value_id = &(data[0]);
    uint8_t *value_data = &(data[1]);

    switch (*value_id) {
        case id_dpi_activepreset: {
            value_data[0] = g_dpi_config.active_preset;
            value_data[1] = 0;
            break;
        }
        case id_dpi_preset0:
        case id_dpi_preset1:
        case id_dpi_preset2:
        case id_dpi_preset3:
        case id_dpi_preset4: {
            uint8_t idx = (uint8_t)(*value_id - id_dpi_preset0);
            uint16_t v = g_dpi_config.presets[idx];
            value_data[0] = v >> 8;
            value_data[1] = v & 0xFF;
            break;
        }
    }
}

static void sleep_config_update(void) {
    eeconfig_update_user_datablock(&g_sleep_config, SLEEP_EEPROM_OFFSET, sizeof(sleep_config_t));
}

/* The aplus.json timeout range is [1, 1440]; because the max is > 255 VIA
   encodes it as 2 BIG-endian bytes (MSB first) -- identical to the offset /
   limit / DPI-preset controls above. The enabled control is a 1-byte toggle
   (options [0, 1], same as id_scroll_accel_enabled).
   Timeout values outside the legal range (e.g. a stale UI sending 0) are
   clamped, never stored: 0 must stay reserved as the EEPROM-uninitialized
   sentinel. */
static void sleep_config_set_value(uint8_t *data) {
    uint8_t *value_id = &(data[0]);
    uint8_t *value_data = &(data[1]);

    switch (*value_id) {
        case id_sleep_timeout_min: {
            uint16_t v = ((uint16_t)value_data[0] << 8) | value_data[1];
            if (v < SLEEP_UI_MIN_MIN) v = SLEEP_UI_MIN_MIN;
            if (v > SLEEP_UI_MAX_MIN) v = SLEEP_UI_MAX_MIN;
            g_sleep_config.timeout_min = v;
            break;
        }
        case id_sleep_enabled: {
            g_sleep_config.enabled = value_data[0] ? 1 : 0;
            /* Switching auto-sleep off also wakes a board that is currently
               asleep (otherwise it would sit dark until the next key press). */
            if (!g_sleep_config.enabled && s_trackball_asleep) {
                trackball_wake_now();
            }
            break;
        }
    }

    /* Persist immediately so a value change survives a reboot even if the host
       does not follow up with an explicit id_custom_save. */
    sleep_config_update();
}

static void sleep_config_get_value(uint8_t *data) {
    uint8_t *value_id = &(data[0]);
    uint8_t *value_data = &(data[1]);

    switch (*value_id) {
        case id_sleep_timeout_min: {
            uint16_t v = g_sleep_config.timeout_min;
            value_data[0] = v >> 8;     /* big-endian, matches the set path */
            value_data[1] = v & 0xFF;
            break;
        }
        case id_sleep_enabled: {
            value_data[0] = g_sleep_config.enabled ? 1 : 0;
            value_data[1] = 0;
            break;
        }
    }
}

/* Sensor channel: the sampling toggle is writable (binary 0/1); every value
   display is a VIA `label` control, so GET must return a null-terminated
   ASCII decimal string (e.g. "87") that the web UI decodes as UTF-8.
   Quality score (0..100) is computed from the session averages via
   calculate_pmw3360_quality_score(). */
static void sensor_write_ascii(uint8_t *value_data, uint16_t val) {
    char    tmp[6];
    uint8_t n = 0;
    do {
        tmp[n++] = '0' + (val % 10);
        val /= 10;
    } while (val && n < sizeof(tmp));
    uint8_t i = 0;
    while (n) {
        value_data[i++] = tmp[--n];
    }
    value_data[i] = '\0';
}

static void sensor_config_set_value(uint8_t *data) {
    uint8_t *value_id   = &(data[0]);
    uint8_t *value_data = &(data[1]);

    switch (*value_id) {
        case id_sensor_sampling: {
            bool on = value_data[0] ? 1 : 0;
            if (on && !s_sensor_sampling) {
                /* Fresh session: drop any previous averages. */
                s_squal_sum         = 0;
                s_shutter_sum       = 0;
                s_pixsum_sum        = 0;
                s_max_sum           = 0;
                s_min_sum           = 0;
                s_sample_count      = 0;
                s_sensor_sample_last = timer_read32();
            }
            s_sensor_sampling = on;
            break;
        }
        /* Label displays are read-only: VIA never SETs them; ignore anyway. */
    }
}

static void sensor_config_get_value(uint8_t *data) {
    uint8_t *value_id   = &(data[0]);
    uint8_t *value_data = &(data[1]);

    switch (*value_id) {
        case id_sensor_quality: {
            uint16_t avg_squal   = s_sample_count ? (uint16_t)(s_squal_sum / s_sample_count) : 0;
            uint16_t avg_shutter = s_sample_count ? (uint16_t)(s_shutter_sum / s_sample_count) : 0;
            uint16_t avg_max     = s_sample_count ? (uint16_t)(s_max_sum / s_sample_count) : 0;
            uint16_t avg_min     = s_sample_count ? (uint16_t)(s_min_sum / s_sample_count) : 0;
            uint16_t avg_pixsum  = s_sample_count ? (uint16_t)(s_pixsum_sum / s_sample_count) : 0;
            uint8_t  score       = calculate_pmw3360_quality_score(
                (uint8_t)avg_squal, avg_shutter,
                (uint8_t)avg_max, (uint8_t)avg_min, (uint8_t)avg_pixsum);
            sensor_write_ascii(value_data, score);
            break;
        }
        case id_sensor_squal:
            sensor_write_ascii(value_data,
                               s_sample_count ? (uint16_t)(s_squal_sum / s_sample_count) : 0);
            break;
        case id_sensor_shutter:
            sensor_write_ascii(value_data,
                               s_sample_count ? (uint16_t)(s_shutter_sum / s_sample_count) : 0);
            break;
        case id_sensor_max:
            sensor_write_ascii(value_data,
                               s_sample_count ? (uint16_t)(s_max_sum / s_sample_count) : 0);
            break;
        case id_sensor_min:
            sensor_write_ascii(value_data,
                               s_sample_count ? (uint16_t)(s_min_sum / s_sample_count) : 0);
            break;
        case id_sensor_pixsum:
            sensor_write_ascii(value_data,
                               s_sample_count ? (uint16_t)(s_pixsum_sum / s_sample_count) : 0);
            break;
        case id_sensor_sampling:
            value_data[0] = s_sensor_sampling ? 1 : 0;
            value_data[1] = 0;
            break;
    }
}

/* Keyboard-level handler for the scroll-accel custom channel. Unhandled channels
   are passed back as id_unhandled (matches the core VIA dispatch contract). */
void via_custom_value_command_kb(uint8_t *data, uint8_t length) {
    (void)length;  // VIA passes the packet length; we index by fixed offsets.
    uint8_t *command_id = &(data[0]);
    uint8_t *channel_id = &(data[1]);
    uint8_t *value_and_data = &(data[2]);

    if (*channel_id == id_led) {
        switch (*command_id) {
            case id_custom_set_value:
                led_config_set_value(value_and_data);
                break;
            case id_custom_get_value:
                led_config_get_value(value_and_data);
                break;
            case id_custom_save:
                led_config_update();
                break;
            default:
                *command_id = id_unhandled;
                break;
        }
        return;
    }

    if (*channel_id == id_scroll_accel) {
        switch (*command_id) {
            case id_custom_set_value:
                scroll_accel_config_set_value(value_and_data);
                break;
            case id_custom_get_value:
                scroll_accel_config_get_value(value_and_data);
                break;
            case id_custom_save:
                scroll_accel_config_update();
                eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));
                drag_scroll_config_update();
                break;
            default:
                *command_id = id_unhandled;
                break;
        }
        scroll_accel_config_changed(); // 标记参数已改，下次滚动自动刷新查表
        return;
    }

    if (*channel_id == id_dpi) {
        switch (*command_id) {
            case id_custom_set_value:
                dpi_config_set_value(value_and_data);
                break;
            case id_custom_get_value:
                dpi_config_get_value(value_and_data);
                break;
            case id_custom_save:
                dpi_config_update();
                break;
            default:
                *command_id = id_unhandled;
                break;
        }
        return;
    }

    if (*channel_id == id_sleep) {
        switch (*command_id) {
            case id_custom_set_value:
                sleep_config_set_value(value_and_data);
                break;
            case id_custom_get_value:
                sleep_config_get_value(value_and_data);
                break;
            case id_custom_save:
                sleep_config_update();
                break;
            default:
                *command_id = id_unhandled;
                break;
        }
        return;
    }

    if (*channel_id == id_sensor) {
        switch (*command_id) {
            case id_custom_set_value:
                sensor_config_set_value(value_and_data);
                break;
            case id_custom_get_value:
                sensor_config_get_value(value_and_data);
                break;
            case id_custom_save:
                break;
            default:
                *command_id = id_unhandled;
                break;
        }
        return;
    }

    *command_id = id_unhandled;
}
