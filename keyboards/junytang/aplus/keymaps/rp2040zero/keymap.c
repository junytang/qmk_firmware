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
#include "../../gesture_mode.h"

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
const rgblight_segment_t PROGMEM righty_nav_layer_colour[] =        RGBLIGHT_LAYER_SEGMENTS( {0, 2, HSV_NAVBLUE} );
const rgblight_segment_t PROGMEM lefty_nav_layer_colour[] =         RGBLIGHT_LAYER_SEGMENTS( {0, 2, HSV_NAVGREEN} );
const rgblight_segment_t PROGMEM control_layer_colour[] =           RGBLIGHT_LAYER_SEGMENTS( {0, 2, HSV_RED} );
const rgblight_segment_t PROGMEM gesture_layer_colour[] =           RGBLIGHT_LAYER_SEGMENTS( {0, 2, HSV_GESTUREYELLOW} );
const rgblight_segment_t PROGMEM option_changed_layer_colour[] =    RGBLIGHT_LAYER_SEGMENTS( {0, 2, HSV_OPTIONCHANGED} );

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

scroll_accel_config_t g_scroll_accel_config = {
    .enabled = 1,
    .offset = 160.0f,
    .takeoff = 0.03f,
    .growth_rate = 0.03f,
    .limit = 1.0f,
    .limit_upper = 4.0f,
};

dpi_config_t g_dpi_config = {
    .presets = { 800, 1200, 1600, 2000, 2400 },
    .active_preset = 1,
};

/* Both persistent structs live in the EECONFIG user-data block
   (EECONFIG_USER_DATA_SIZE, config.h), via eeconfig_read_user_datablock /
   eeconfig_update_user_datablock -- matching users/junytang/ploopy_via.c.
   Once EECONFIG_USER_DATA_SIZE > 0 the legacy eeconfig_read_user /
   eeconfig_update_user are unavailable, so user_config and the scroll config
   must both use the datablock API. user_config occupies the first bytes; the
   scroll config follows it. This block is separate from the VIA custom-config
   region, so it does not collide with the drashna/pointing_device_accel module. */
#define USER_CONFIG_EEPROM_OFFSET 0
#define SCROLL_ACCEL_EEPROM_OFFSET (USER_CONFIG_EEPROM_OFFSET + sizeof(user_config_t))
#define DPI_EEPROM_OFFSET (SCROLL_ACCEL_EEPROM_OFFSET + sizeof(scroll_accel_config_t))
_Static_assert(SCROLL_ACCEL_EEPROM_OFFSET + sizeof(scroll_accel_config_t) <= EECONFIG_USER_DATA_SIZE,
                "user_config + scroll_accel_config exceed EECONFIG_USER_DATA_SIZE; grow it in config.h");
_Static_assert(DPI_EEPROM_OFFSET + sizeof(dpi_config_t) <= EECONFIG_USER_DATA_SIZE,
                "user_config + scroll_accel_config + dpi_config exceed EECONFIG_USER_DATA_SIZE; grow it in config.h");

/* Per-param real ranges for VIA projection (must match aplus.json "options"). */
#define SCROLL_ACCEL_R_MIN_OFFSET 0.0f
#define SCROLL_ACCEL_R_MAX_OFFSET 400.0f
#define SCROLL_ACCEL_R_MIN_TAKEOFF 0.005f
#define SCROLL_ACCEL_R_MAX_TAKEOFF 0.1f
#define SCROLL_ACCEL_R_MIN_GROWTH_RATE 0.001f
#define SCROLL_ACCEL_R_MAX_GROWTH_RATE 0.1f
#define SCROLL_ACCEL_R_MIN_LIMIT 0.5f
#define SCROLL_ACCEL_R_MAX_LIMIT 1.0f
#define SCROLL_ACCEL_R_MIN_LIMIT_UPPER 1.0f
#define SCROLL_ACCEL_R_MAX_LIMIT_UPPER 10.0f

void scroll_accel_rebuild_lut(void);
static void scroll_accel_config_update(void);

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

/* Override default function in submodule code so that we can blink whenever
   a successful gesture is registered. */
void pointing_device_gestures_trigger(uint8_t direction) {

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

    /* Keymap. */
    const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
        // Base layer with all of the mouse-related stuff for everyday use.
        [LAYER_NAV_RIGHT_HANDED] = LAYOUT(  MS_BTN6, MS_BTN5, PKC_DRAG_SCROLL, MS_BTN4,
                                        MS_BTN1, MS_BTN3,
                                        MS_BTN2, TD(TD_CONTROL_GESTURE) ),
        // Mirror-image of right-handed layout for lefties.
        [LAYER_NAV_LEFT_HANDED] = LAYOUT(   LT(3, MS_BTN4), PKC_DRAG_SCROLL, MS_BTN5, MS_BTN6,
                                        MS_BTN3, MS_BTN1,
                                        TD(TD_CONTROL_GESTURE), MS_BTN2 ),
    // (The control/gesture button is now the 3-in-1 TD_CONTROL_GESTURE: tap = gesture_tap_keycode
    //  (MS_BTN7 default), hold = MO(LAYER_CONTROL), tap+hold = gesture.)
    // Layer for all of the customization options.
    [LAYER_CONTROL] = LAYOUT(           PKC_BLINKY_DPI_CONFIG, PKC_ADJUST_LED_BRIGHTNESS, PKC_TGL_VERT_SCRL, PKC_TGL_HORIZ_SCRL,
                                        PKC_TGL_MIRROR, PKC_TGL_DRAG_SCRL,
                                        MO(LAYER_CONTROL), MO(LAYER_CONTROL) ),
    [3] = LAYOUT( _______, _______, _______, _______, _______, _______, _______, _______ ),
    [4] = LAYOUT( _______, _______, _______, _______, _______, _______, _______, _______ ),
    [5] = LAYOUT( _______, _______, _______, _______, _______, _______, _______, _______ ),
    [6] = LAYOUT( _______, _______, _______, _______, _______, _______, _______, _______ ),
    [7] = LAYOUT( _______, _______, _______, _______, _______, _______, _______, _______ )
};

/* 3-in-1 control/gesture key (TD_CONTROL_GESTURE), defined in gesture_mode.c:
 *   single tap    -> user_config.gesture_tap_keycode (MS_BTN7 default, VIA-settable)
 *   single hold   -> MO(LAYER_CONTROL) for the duration of the hold
 *   tap then hold -> pointing-device gesture mode */
tap_dance_action_t tap_dance_actions[] = {
    [TD_CONTROL_GESTURE] = { .fn = { .on_each_tap       = cg_press,
                                     .on_dance_finished = cg_fin,
                                     .on_reset          = cg_rst },
                              .user_data = (void *)&user_config.gesture_tap_keycode },
};

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

    user_config.left_handed = true;                // Right-handed.
    user_config.horizontal_scroll_arrows = false;   // False = Scroll events, True = left/right arrows.
    user_config.vertical_scroll_arrows = false;     // False = Scroll events, True = up/down arrows.
    user_config.drag_scroll_mode = false;           // Hold to activate.
    user_config.led_brightness = 4;                 // Brightest mode by default.
    user_config.gesture_tap_keycode = MS_BTN7;    // Tap of the gesture key.

    eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));

    /* Persist the scroll-accel defaults into the EECONFIG user-data block.
       Set them explicitly here (not only via the global initializer) so a
       factory-init / wipe can never leave k or max_boost as 0. */
    g_scroll_accel_config = (scroll_accel_config_t){
        .enabled = 1,
        .offset = 160.0f,
        .takeoff = 0.03f,
        .growth_rate = 0.03f,
        .limit = 1.0f,
        .limit_upper = 4.0f,
    };
    scroll_accel_config_update();

    /* Seed DPI presets + active preset into the EECONFIG user-data block too. */
    g_dpi_config = (dpi_config_t){
        .presets = { 800, 1200, 1600, 2000, 2400 },
        .active_preset = 1,
    };
    dpi_config_update();
}

void keyboard_pre_init_user(void) {

    /* Set up GPIO for both TMAG sensors. */
    gpio_set_pin_output_push_pull(TMAG5273_D0_PWR_PIN);
    gpio_set_pin_output_push_pull(TMAG5273_D1_PWR_PIN);
    gpio_write_pin_low(TMAG5273_D0_PWR_PIN);
    gpio_write_pin_low(TMAG5273_D1_PWR_PIN);

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
       reseed the board defaults and persist them so the gesture keycode and
       lighting stay sane. */
    if (user_config.raw == 0xFFFFFFFF || user_config.led_brightness > 5) {
        user_config.raw = 0;
        user_config.left_handed = true;
        user_config.horizontal_scroll_arrows = false;
        user_config.vertical_scroll_arrows = false;
        user_config.drag_scroll_mode = false;
        user_config.led_brightness = 4;
        user_config.gesture_tap_keycode = MS_BTN7;
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
            .offset = 160.0f,
            .takeoff = 0.03f,
            .growth_rate = 0.03f,
            .limit = 1.0f,
            .limit_upper = 4.0f,
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
}

bool process_record_user(uint16_t keycode, keyrecord_t* record) {
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

#define ACCEL_LUT_SIZE 128 // 覆盖 speed 从 0 到 127 (足以应对任何极速甩动)

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

uint32_t leftwheel_timeout = 0;
uint32_t rightwheel_timeout = 0;

report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    // throttle reads
    if (timer_elapsed32(last_scroll_time) > 20) {
        uint16_t leftwheel_rawangle = tmag5273_get_angle(TMAG5273_D0_I2C_ADDRESS);
        uint16_t rightwheel_rawangle = tmag5273_get_angle(TMAG5273_D1_I2C_ADDRESS);

        int16_t leftwheel_deadzone_distance = calculate_deadzone_distance( leftwheel_rawangle, &leftwheel_deadzone_center );
        int16_t rightwheel_deadzone_distance = calculate_deadzone_distance( rightwheel_rawangle, &rightwheel_deadzone_center );

        int16_t leftwheel_delta = calculate_wheel_delta( leftwheel_rawangle, leftwheel_current_position );
        leftwheel_current_position = leftwheel_rawangle;

        int16_t rightwheel_delta = calculate_wheel_delta( rightwheel_rawangle, rightwheel_current_position );
        rightwheel_current_position = rightwheel_rawangle;

        /* If either wheel moved, we reset the timeout counter. Else, we start
           counting inactivity. */
        if( abs(leftwheel_delta) > 2 ) {
            leftwheel_timeout = 0;
        }
        else {
            /* Don't increment forever, you'll get rollover. */
            if( leftwheel_timeout < TMAG5273_DEADZONE_PROTECTOR_TIMEOUT ) {
                leftwheel_timeout += timer_elapsed32(last_scroll_time);
            }
        }
        if( abs(rightwheel_delta) > 2 ) {
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

            leftwheel_volume_scroll_tick += leftwheel_delta;
            rightwheel_volume_scroll_tick += rightwheel_delta;

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

            leftwheel_arrow_scroll_tick += leftwheel_delta;
            rightwheel_arrow_scroll_tick += rightwheel_delta;

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

            leftwheel_arrow_scroll_tick += leftwheel_delta;
            rightwheel_arrow_scroll_tick += rightwheel_delta;

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
           While inside the deadzone the residual is reset to zero, so the motion
           made before crossing is intentionally discarded (anti-jitter: no scroll
           near rest) and the scroll count starts fresh from zero at the crossing
           point -- never a burst of accumulated startup motion. The wheel->axis
           mapping is identical for hi-res and low-res OSes; only the divisor
           differs, so the deadzone gate applies uniformly (incl. macOS). */
        bool hi_res = (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX);

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
                    mouse_report.h = scroll_accel_apply(&leftwheel_scroll_acc, leftwheel_delta, left_div);
                }
            } else {
                mouse_report.v = -scroll_accel_apply(&leftwheel_scroll_acc, leftwheel_delta, left_div);
            }
        }

        if ( right_outside ) {
            if ( right_is_h ) {
                if ( !user_config.horizontal_scroll_arrows ) {
                    mouse_report.h = scroll_accel_apply(&rightwheel_scroll_acc, rightwheel_delta, right_div);
                }
            } else {
                mouse_report.v = -scroll_accel_apply(&rightwheel_scroll_acc, rightwheel_delta, right_div);
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
    }

    /* Persist immediately so a value change survives a reboot even if the host
       does not follow up with an explicit id_custom_save. */
    scroll_accel_config_update();
    eeconfig_update_user_datablock(&user_config, USER_CONFIG_EEPROM_OFFSET, sizeof(user_config));
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
    }
}

static void scroll_accel_config_update(void) {
    eeconfig_update_user_datablock(&g_scroll_accel_config, SCROLL_ACCEL_EEPROM_OFFSET, sizeof(scroll_accel_config_t));
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

/* Keyboard-level handler for the scroll-accel custom channel. Unhandled channels
   are passed back as id_unhandled (matches the core VIA dispatch contract). */
void via_custom_value_command_kb(uint8_t *data, uint8_t length) {
    (void)length;  // VIA passes the packet length; we index by fixed offsets.
    uint8_t *command_id = &(data[0]);
    uint8_t *channel_id = &(data[1]);
    uint8_t *value_and_data = &(data[2]);

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

    *command_id = id_unhandled;
}
