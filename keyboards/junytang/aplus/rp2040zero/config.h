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

#undef PLOOPY_DRAGSCROLL_DIVISOR_H
#undef PLOOPY_DRAGSCROLL_DIVISOR_V
#define PLOOPY_DRAGSCROLL_DIVISOR_H 32
#define PLOOPY_DRAGSCROLL_DIVISOR_V 32

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

/* user_config (gesture keycode + board settings, 4 bytes), the scroll-accel
   config (1 bool + 5 floats, 24 bytes with padding) and the DPI config
   (5x uint16 presets + uint8 active preset = 11 bytes) are all persisted in the
   EECONFIG user-data block via eeconfig_read_user_datablock /
   eeconfig_update_user_datablock in keymap.c, following the pattern in
   users/junytang/ploopy_via.c. This keeps them out of the VIA custom-config
   EEPROM region, which the drashna/pointing_device_accel module (enabled in
   keymap.json) owns for its cursor-accel data -- so we deliberately do NOT
   redefine VIA_EEPROM_CUSTOM_CONFIG_SIZE, avoiding that module's
   "already defined" #error / per-build-unit #pragma message.
   Layout: user_config at offset 0 (4B), scroll config at offset 4 (24B),
   DPI config at offset 28 (11B) => 39B used.
   This must be >= 39; grow it if any struct grows. After flashing a build that
   changes this, do a one-time EEPROM reset so eeconfig_init_user repopulates
   everything with defaults. */
#define EECONFIG_USER_DATA_SIZE 40
