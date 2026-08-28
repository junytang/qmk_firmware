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

#include QMK_KEYBOARD_H
#include "pointing_device_gestures.h"

/* EEPROM-backed board settings. The gesture tap keycode lives here so it can
 * be changed (default KC_MS_BTN2) without rewriting the keycode tables. */
typedef union {
    uint32_t raw;
    struct {
        bool    left_handed :1;
        bool    horizontal_scroll_arrows :1;
        bool    vertical_scroll_arrows :1;
        bool    drag_scroll_mode :1;
        uint8_t led_brightness: 8;
        uint16_t gesture_tap_keycode;
    };
} user_config_t;

extern user_config_t user_config;

/* Layer indices, shared so gesture_mode.c can reference LAYER_CONTROL. */
enum {
    LAYER_NAV_RIGHT_HANDED = 0,
    LAYER_NAV_LEFT_HANDED,
    LAYER_CONTROL
};

/* 3-in-1 control/gesture key (TD_CONTROL_GESTURE): tap = the configurable
 * gesture_tap_keycode (MS_BTN7 default, VIA-settable), hold = MO(LAYER_CONTROL),
 * tap+hold (double-tap) = pointing-device gesture mode. */
enum {
    TD_CONTROL_GESTURE = 0,
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
