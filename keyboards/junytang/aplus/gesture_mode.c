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

#include "gesture_mode.h"

static bool cg_layer_active = false;
static bool cg_gesture_active = false;

static int cg_cur_dance(tap_dance_state_t *state) {
    if (state->count == 1) {
      if (state->interrupted || !state->pressed)  return SINGLE_TAP;
      //key has not been interrupted, but they key is still held. Means you want to send a 'HOLD'.
      else return SINGLE_HOLD;
    }
    else if (state->count == 2) {
      /*
      * DOUBLE_SINGLE_TAP is to distinguish between typing "pepper", and actually wanting a double tap
      * action when hitting 'pp'. Suggested use case for this return value is when you want to send two
      * keystrokes of the key, and not the 'double tap' action/macro.
      */
      if (state->interrupted) return DOUBLE_SINGLE_TAP;
      else if (state->pressed) return DOUBLE_HOLD;
      else return DOUBLE_TAP;
    }
    //Assumes no one is trying to type the same letter three times (at least not quickly).
    //If your tap dance key is 'KC_W', and you want to type "www." quickly - then you will need to add
    //an exception here to return a 'TRIPLE_SINGLE_TAP', and define that enum just like 'DOUBLE_SINGLE_TAP'
    if (state->count == 3) {
      if (state->interrupted || !state->pressed)  return TRIPLE_TAP;
      else return TRIPLE_HOLD;
    }
    else return 8; //magic number. At some point this method will expand to work for more presses
}

/* The gesture is started in cg_fin once a DOUBLE_HOLD is resolved, so the
 * different press methods stay clearly separated (see cg_cur_dance). */
void cg_press(tap_dance_state_t *state, void *user_data) {
}

void cg_fin(tap_dance_state_t *state, void *user_data) {
    /* user_data points at the configurable tap keycode (user_config.gesture_tap_keycode). */
    uint16_t tap_kc = *(const uint16_t *)user_data;
    switch (cg_cur_dance(state)) {
        case SINGLE_TAP:
            tap_code16(tap_kc);
            break;
        case SINGLE_HOLD:
            pointing_device_gestures_start();
            cg_gesture_active = true;
            break;
        case DOUBLE_HOLD:
            /* tap then hold: run the pointing-device gesture until the key is
             * released (cleaned up in cg_rst via cg_gesture_active). */
            layer_on(LAYER_CONTROL);
            cg_layer_active = true;
            break;
        default:
            break;
    }
}

void cg_rst(tap_dance_state_t *state, void *user_data) {
    if (cg_layer_active) {
        layer_off(LAYER_CONTROL);
        cg_layer_active = false;
    }
    if (cg_gesture_active) {
        pointing_device_gestures_end();
        cg_gesture_active = false;
    }
}
