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

enum {
    GESTURE_SINGLE_TAP = 1,
    GESTURE_SINGLE_HOLD = 2,
};

static int gesture_cur_dance(tap_dance_state_t *state) {
    if (state->count == 1) {
        if (state->interrupted || !state->pressed) return GESTURE_SINGLE_TAP;
        /* Key still held past the tapping term: it's a hold, not a tap. */
        return GESTURE_SINGLE_HOLD;
    }
    return 0;
}

/* Start accumulating gesture movement the instant the key is pressed, so fast
 * flicks are captured without waiting for the tapping term. */
void gesturetd_press(tap_dance_state_t *state, void *user_data) {
    pointing_device_gestures_start();
}

void gesturetd_fin(tap_dance_state_t *state, void *user_data) {
    /* user_data points at the configurable tap keycode (user_config.gesture_tap_keycode). */
    uint16_t kc = *(const uint16_t *)user_data;
    switch (gesture_cur_dance(state)) {
        case GESTURE_SINGLE_TAP:
            tap_code16(kc);
            break;
        case GESTURE_SINGLE_HOLD:
            /* Held: gesture mode keeps running; the direction fires on release. */
        default:
            break;
    }
}

void gesturetd_rst(tap_dance_state_t *state, void *user_data) {
    pointing_device_gestures_end();
}
