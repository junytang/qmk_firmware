/* Copyright 2025 Jun Y. Tang
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

#pragma once

#include <stddef.h> // offsetof, for the layout asserts below
#include QMK_KEYBOARD_H
#include "pointing_device_gestures.h"

/* EEPROM-backed board settings. Both configurable tap keycodes live here so
 * they can be changed (gesture = MS_BTN7, drag-scroll = MS_BTN8) without
 * rewriting the keycode tables.
 *
 * NOTE: this union is 8 bytes -- the two 16-bit keycodes do not fit in 32 bits
 * alongside the other fields. Growing it shifts the scroll-accel / DPI blocks,
 * so EECONFIG_USER_DATA_SIZE (rp2040zero/config.h) has to be bumped to match.
 *
 * The inner struct only occupies 6 bytes (2 of bitfields + 2x uint16), so the
 * trailing 2 bytes are spare: the two knob-invert flags live there. Adding
 * :1 fields keeps led_brightness at byte 1 and both keycodes at offsets 2/4,
 * so NO downstream block moves and existing EEPROM keeps its meaning (old
 * padding bytes read as 0 = "off", which is exactly the default). */
typedef union {
    uint64_t raw;
    struct {
        bool    left_handed :1;
        bool    horizontal_scroll_arrows :1;
        bool    vertical_scroll_arrows :1;
        bool    drag_scroll_mode :1;
        bool    left_knob_invert :1;
        bool    right_knob_invert :1;
        uint8_t led_brightness: 8;
        uint16_t gesture_tap_keycode;
        uint16_t drag_scroll_tap_keycode;
    };
} user_config_t;

_Static_assert(sizeof(user_config_t) == 8, "user_config_t must stay 8 bytes; growing it shifts every EECONFIG user block (bump EECONFIG_USER_DATA_SIZE and its VERSION in rp2040zero/config.h)");
_Static_assert(offsetof(user_config_t, gesture_tap_keycode) == 2, "gesture_tap_keycode moved; existing EEPROM keycodes would be misread");
_Static_assert(offsetof(user_config_t, drag_scroll_tap_keycode) == 4, "drag_scroll_tap_keycode moved; existing EEPROM keycodes would be misread");

extern user_config_t user_config;

/* Layer indices, shared so tap_dance.c can reference LAYER_CONTROL. */
enum {
    LAYER_NAV_RIGHT_HANDED = 0,
    LAYER_NAV_LEFT_HANDED,
    LAYER_CONTROL
};

/* Tap dances. TD_CONTROL_GESTURE: tap = the configurable gesture_tap_keycode
 * (MS_BTN7 default, VIA-settable), hold = pointing-device gesture mode,
 * tap+hold = MO(LAYER_CONTROL).
 * TD_DRAG_SCROLL: tap = the configurable drag_scroll_tap_keycode (MS_BTN8
 * default, VIA-settable), hold = drag scroll. */
enum {
    TD_CONTROL_GESTURE = 0,
    TD_DRAG_SCROLL,
};

enum {
    SINGLE_TAP = 1,
    SINGLE_HOLD = 2,
    DOUBLE_TAP = 3,
    DOUBLE_HOLD = 4,
    DOUBLE_SINGLE_TAP = 5, //send two single taps
    TRIPLE_TAP = 6,
    TRIPLE_HOLD = 7
};

void cg_press(tap_dance_state_t *state, void *user_data);
void cg_fin(tap_dance_state_t *state, void *user_data);
void cg_rst(tap_dance_state_t *state, void *user_data);

void ds_press(tap_dance_state_t *state, void *user_data);
void ds_fin(tap_dance_state_t *state, void *user_data);
void ds_rst(tap_dance_state_t *state, void *user_data);
