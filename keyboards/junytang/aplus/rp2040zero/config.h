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

#pragma once

#undef UNUSABLE_PINS
#define UNUSABLE_PINS \
    { GP5, GP10, GP11, GP17, GP18, GP19, GP20, GP21, GP22, GP23, GP24, GP25 }

#undef POINTING_DEVICE_INVERT_Y
#define POINTING_DEVICE_INVERT_X

#undef SPI_DRIVER
#undef SPI_SCK_PIN
#undef SPI_MOSI_PIN
#undef SPI_MISO_PIN
#undef PMW33XX_CS_PIN
#define SPI_DRIVER SPID1
#define SPI_SCK_PIN GP14
#define SPI_MOSI_PIN GP15
#define SPI_MISO_PIN GP12
#define PMW33XX_CS_PIN GP9 // SPI CS pin.

#undef I2C_DRIVER
#undef I2C1_SDA_PIN
#undef I2C1_SCL_PIN
#define I2C_DRIVER I2CD1
#define I2C1_SDA_PIN GP6
#define I2C1_SCL_PIN GP7

#undef TMAG5273_D0_PWR_PIN
#undef TMAG5273_D1_PWR_PIN
#define TMAG5273_D0_PWR_PIN GP26
#define TMAG5273_D1_PWR_PIN GP4


/* user_config (gesture keycode + board settings, 8 bytes), the scroll-accel
   config (1 bool + 5 floats, 24 bytes with padding), the DPI config
   (5x uint16 presets + uint8 active preset = 12 bytes with padding), the
   auto-sleep config (uint16 minutes + uint8 enabled = 4 bytes with padding),
   the drag-scroll config (2 floats, 8 bytes) and the custom LED config
   (righty/lefty hue & sat, 4 bytes) are all persisted in the EECONFIG
   user-data block via eeconfig_read_user_datablock / eeconfig_update_user_datablock
   in keymap.c, following the pattern in users/junytang/ploopy_via.c.

   Layout:
     user_config at offset 0 (8B)
   + scroll config at offset 8 (24B)
   + DPI config at offset 32 (12B)
   + sleep config at offset 44 (4B)
   + drag-scroll config at offset 48 (8B)
   + led config at offset 56 (4B)
   => 60B used total.

   Bumped 40 -> 48 when user_config_t grew;
   48 -> 56 when drag_scroll_config_t was appended;
   56 -> 64 when led_config_t was appended (aligned to 64B with 4B headroom).
   The static asserts in keymap.c fail loudly if this ever needs raising. */
#define EECONFIG_USER_DATA_SIZE 64

/* Pin the datablock version to the OLD size (48) so the append-only 48 -> 56 -> 64
   growth does not invalidate the block. nvm_eeconfig treats a version
   mismatch as "wipe the whole datablock", which would reseed every guard and
   throw away the user's tuned scroll/DPI/sleep values; keeping the version
   at 48 means the first 48 bytes keep their exact old meaning and only the
   fresh tail reads as uninitialized (caught by the respective guards in
   keyboard_post_init_user, which reseed just the new structs).
   Bump this only if an EXISTING struct is reordered/resized -- appending new
   structs at the end must leave it alone. */
#define EECONFIG_USER_DATA_VERSION 48
