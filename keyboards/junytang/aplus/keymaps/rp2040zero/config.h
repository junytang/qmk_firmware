/* Copyright 2020 Christopher Courtney, aka Drashna Jael're  (@drashna) <drashna@live.com>
 * Copyright 2019 Sunjun Kim
 * Copyright 2020 Ploopy Corporation
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

#define DYNAMIC_KEYMAP_LAYER_COUNT 8

/* Trackball auto-sleep. After TRACKBALL_SLEEP_TIMEOUT_MS with no key press,
   ball/button motion or wheel movement, the trackball stops tracking: the
   cursor is frozen and the TMAG5273 wheel I2C polls are skipped entirely.
   Any key press wakes it. */
#define TRACKBALL_SLEEP_TIMEOUT_MS 120 * 60 * 1000 // 120 minutes of idle before sleeping
