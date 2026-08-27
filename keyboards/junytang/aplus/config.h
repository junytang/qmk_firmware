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

#define UNUSABLE_PINS \
    { GP5, GP10, GP11, GP17, GP18, GP19, GP20, GP21, GP22, GP23, GP24, GP25 }

#define POINTING_DEVICE_HIRES_SCROLL_ENABLE
#define POINTING_DEVICE_HIRES_SCROLL_MULTIPLIER 120
#define POINTING_DEVICE_HIRES_SCROLL_EXPONENT 0

#define POINTING_DEVICE_INVERT_X

#define MOUSE_EXTENDED_REPORT
#define WHEEL_EXTENDED_REPORT

/* How far you have to move the cursor to register a gesture. */
#define POINTING_DEVICE_GESTURES_THRESHOLD 40

/* Uncomment these lines for pointing device acceleration. */
#define POINTING_DEVICE_ACCEL_TAKEOFF 2.0      // lower/higher value = curve takes off more smoothly/abruptly
#define POINTING_DEVICE_ACCEL_GROWTH_RATE 0.25 // lower/higher value = curve reaches its upper limit slower/faster
#define POINTING_DEVICE_ACCEL_OFFSET 2.2       // lower/higher value = acceleration kicks in earlier/later
#define POINTING_DEVICE_ACCEL_LIMIT 0.2        // lower limit of accel curve (minimum acceleration factor)

#define PLOOPY_DRAGSCROLL_MOMENTARY
#define PLOOPY_DRAGSCROLL_DIVISOR_H 32
#define PLOOPY_DRAGSCROLL_DIVISOR_V 32
#define PLOOPY_DRAGSCROLL_INVERT

/* PMW3360 Settings */
#define PMW33XX_LIFTOFF_DISTANCE 0x00
// #define PMW33XX_CS_PIN GP5
// #define SPI_SCK_PIN GP2
// #define SPI_MISO_PIN GP4
// #define SPI_MOSI_PIN GP3
#define SPI_DRIVER SPID1
#define SPI_SCK_PIN GP14
#define SPI_MOSI_PIN GP15
#define SPI_MISO_PIN GP12
#define PMW33XX_CS_PIN GP9 // SPI CS pin.

#define I2C_DRIVER I2CD1
// #define I2C1_SDA_PIN GP12
// #define I2C1_SCL_PIN GP13
#define I2C1_SDA_PIN GP6
#define I2C1_SCL_PIN GP7

/* Increase I2C speed to 400 KHz */
#define F_SCL 400000L

// #define TMAG5273_D0_PWR_PIN GP27
// #define TMAG5273_D1_PWR_PIN GP14
#define TMAG5273_D0_PWR_PIN GP26
#define TMAG5273_D1_PWR_PIN GP4

#define TMAG5273_D0_I2C_ADDRESS (0x50 << 1)
#define TMAG5273_D1_I2C_ADDRESS (0x51 << 1)

#define RGBLIGHT_SLEEP
#define RGBLIGHT_LAYERS_RETAIN_VAL
/* On-board lighting is configurable in 4 steps. This variable configures
   the step size in fractions of full brightness (out of 255). */
#define RGBLIGHT_VAL_STEP 63
#if RGBLIGHT_VAL_STEP > 63
    #error "RGBLIGHT_VAL_STEP must be less than 64."
#endif

/* Define custom colours for effects */
#define HSV_NAVBLUE             180, 220, 252
#define HSV_NAVGREEN            66,  240, 252
#define HSV_GESTUREYELLOW       10,  250, 252
#define HSV_OPTIONCHANGED       240, 250, 252

/* How long to blink for when an option-change button is pressed. */
#define OPTION_CHANGE_BLINK_TIMEOUT 200

/* user_config (gesture keycode + board settings, 4 bytes) and the scroll-accel
   config (4 floats, 16 bytes) are both persisted in the EECONFIG user-data block
   via eeconfig_read_user_datablock / eeconfig_update_user_datablock in keymap.c,
   following the pattern in users/junytang/ploopy_via.c. This keeps them out of the
   VIA custom-config EEPROM region, which the drashna/pointing_device_accel module
   (enabled in keymap.json) owns for its cursor-accel data -- so we deliberately do
   NOT redefine VIA_EEPROM_CUSTOM_CONFIG_SIZE, avoiding that module's
   "already defined" #error / per-build-unit #pragma message.
   Layout: user_config at offset 0 (4B), scroll config at offset 4 (16B) => 20B used.
   This must be >= 20; grow it if either struct grows. After flashing a build that
   changes this (or moves user_config into the block), do a one-time EEPROM reset
   so eeconfig_init_user repopulates both with defaults. */
#define EECONFIG_USER_DATA_SIZE 28
