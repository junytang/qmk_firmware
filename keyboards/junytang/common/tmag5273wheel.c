/* Copyright 2025 Colin Lam, Ploopy Corporation (contact@ploopy.co)
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

#include <stdint.h>
#include <stdbool.h>
#include "printf.h"
#include "tmag5273wheel.h"

/* Switch the I2C backend. With USE_SOFT_I2C (set by SOFT_I2C_DRIVER = yes in
   rules.mk) we use the polled, hang-free bit-bang driver; otherwise the
   hardware I2C driver. All TMAG I/O below goes through tmag_i2c_*. */
#ifdef USE_SOFT_I2C
    #include "soft_i2c.h"
    #define tmag_i2c_init()      soft_i2c_init()
    #define tmag_i2c_ping(a, t)  soft_i2c_ping_address((a), (t))
    #define tmag_i2c_write_reg   soft_i2c_write_register
    #define tmag_i2c_read_reg    soft_i2c_read_register
    #define tmag_i2c_recover()   soft_i2c_recover()
#else
    #define tmag_i2c_init()      i2c_init()
    #define tmag_i2c_ping(a, t)  i2c_ping_address((a), (t))
    #define tmag_i2c_write_reg   i2c_write_register
    #define tmag_i2c_read_reg    i2c_read_register
    #define tmag_i2c_recover()   do {} while (0)
#endif

void tmag5273_init(void) {
    tmag_i2c_init();
}

// Finds a TMAG5273 device on the I2C bus, and initializes it.
// The device_addr argument sets the found device's address to
// device_addr.
void tmag5273_init_device(uint8_t device_addr) {
    uint16_t addr;

    if (tmag_i2c_ping(TMAG5273A1_I2C_ADDRESS, 100) == I2C_STATUS_SUCCESS) {
        addr = TMAG5273A1_I2C_ADDRESS;
    } else if (tmag_i2c_ping(TMAG5273B1_I2C_ADDRESS, 100) == I2C_STATUS_SUCCESS) {
        addr = TMAG5273B1_I2C_ADDRESS;
    } else if (tmag_i2c_ping(TMAG5273C1_I2C_ADDRESS, 100) == I2C_STATUS_SUCCESS) {
        addr = TMAG5273C1_I2C_ADDRESS;
    } else if (tmag_i2c_ping(TMAG5273D1_I2C_ADDRESS, 100) == I2C_STATUS_SUCCESS) {
        addr = TMAG5273D1_I2C_ADDRESS;
    } else {
        // No TMAG5273 device found on the bus; bail out gracefully instead of
        // calling exit(1) (which would brick the board). The wheel simply won't
        // report until a device appears.
        return;
    }

    // Give found device a new device address
    uint8_t i2c_address = device_addr | 0x1;
    tmag_i2c_write_reg(addr, REG_I2C_ADDRESS, &i2c_address, 1, 100);

    // 32x oversampling
    uint8_t device_config_1 = 0b101 << 2;

    // Low noise mode
    // Operating Mode: Continuous Measure Mode
    uint8_t device_config_2 = (0b1 << 4) | (0b10);

    // X and Y channel enabled
    uint8_t sensor_config_1 = 0x30;

    // Enable X and Y angle calculation
    // X and Y magnetic ranges at +-80mT
    uint8_t sensor_config_2 = (0x1 << 2) | (0b10);

    uint8_t int_config = 0x1;

    tmag_i2c_write_reg(device_addr, REG_DEVICE_CONFIG_1, &device_config_1, 1, 100);
    tmag_i2c_write_reg(device_addr, REG_DEVICE_CONFIG_2, &device_config_2, 1, 100);
    tmag_i2c_write_reg(device_addr, REG_SENSOR_CONFIG_1, &sensor_config_1, 1, 100);
    tmag_i2c_write_reg(device_addr, REG_SENSOR_CONFIG_2, &sensor_config_2, 1, 100);
    tmag_i2c_write_reg(device_addr, REG_INT_CONFIG_1, &int_config, 1, 100);
}

uint16_t tmag5273_get_angle(uint8_t device_addr) {
    /* Cache one last-good reading per device so a transient I2C error feeds
       the previous value instead of garbage, and so we only attempt bus
       recovery once we've actually talked to the device. Keyed by address so
       the driver stays generic for any number of wheels. */
    static uint8_t  cache_addr[2] = { 0, 0 };
    static uint16_t cache_val[2]  = { 0, 0 };
    static uint8_t  fail_count    = 0;
    static bool     ever_ok        = false;

    int idx = 0;
    for (int i = 0; i < 2; i++) {
        if (cache_addr[i] == device_addr) { idx = i; break; }
        if (cache_addr[i] == 0)            { cache_addr[i] = device_addr; idx = i; break; }
    }

    uint8_t data[2];
    i2c_status_t st = tmag_i2c_read_reg(device_addr, REG_ANGLE_RESULT_MSB, data, 2, 20);
    if (st != I2C_STATUS_SUCCESS) {
        /* On a glitchy bus, attempt recovery -- but only if we've ever talked
           to a device, so we don't spam recovery when one is simply absent. */
        if (ever_ok && ++fail_count >= 3) {
            tmag_i2c_recover();
            fail_count = 0;
        }
        return cache_val[idx];
    }

    ever_ok    = true;
    fail_count = 0;
    uint16_t rawangle = (uint16_t)((data[0] << 8) | data[1]);
    cache_val[idx] = rawangle;
    return rawangle;
}

/* Calculate the distance between two wheel positions. Takes into account wraparound and the
   size of the wheel. Positive values are clockwise distances, negative values CCW. */
int16_t calculate_wheel_delta( uint16_t wheel_newangle, uint16_t wheel_oldangle ) {
    int16_t wheel_delta = (int16_t)(wheel_newangle - wheel_oldangle);

    if (wheel_delta > (TMAG5273_MAX_SENSOR_VALUE / 2)) {
        wheel_delta -= TMAG5273_MAX_SENSOR_VALUE;
    } else if (wheel_delta < -(TMAG5273_MAX_SENSOR_VALUE / 2)) {
        wheel_delta += TMAG5273_MAX_SENSOR_VALUE;
    }

    return wheel_delta;
}

/* Returns the angular distance bewteen wheel_rawangle and _wheel_deadzone_center. Also updates
   wheel_deadzone_center whenever there's a reason to move the deadzone. */
int16_t calculate_deadzone_distance( uint16_t wheel_rawangle, uint16_t* wheel_deadzone_center ) {
    uint16_t wheel_deadzone_lower = ( wheel_rawangle -
                                          TMAG5273_WHEEL_DEADZONE +
                                          TMAG5273_MAX_SENSOR_VALUE)
                                        % TMAG5273_MAX_SENSOR_VALUE;
    uint16_t wheel_deadzone_upper = ( wheel_rawangle +
                                          TMAG5273_WHEEL_DEADZONE +
                                          TMAG5273_MAX_SENSOR_VALUE)
                                        % TMAG5273_MAX_SENSOR_VALUE;

    int16_t wheel_deadzone_distance = ( wheel_rawangle -
                                  *wheel_deadzone_center +
                                  TMAG5273_MAX_SENSOR_VALUE)
                                % TMAG5273_MAX_SENSOR_VALUE;

    if (wheel_deadzone_distance > (TMAG5273_MAX_SENSOR_VALUE / 2)) {
        wheel_deadzone_distance -= TMAG5273_MAX_SENSOR_VALUE;
    } else if (wheel_deadzone_distance < -(TMAG5273_MAX_SENSOR_VALUE / 2)) {
        wheel_deadzone_distance += TMAG5273_MAX_SENSOR_VALUE;
    }

    if( wheel_deadzone_distance > TMAG5273_WHEEL_DEADZONE ) {
        *wheel_deadzone_center = wheel_deadzone_lower;
    }
    if( wheel_deadzone_distance < -TMAG5273_WHEEL_DEADZONE ) {
        *wheel_deadzone_center = wheel_deadzone_upper;
    }

    return wheel_deadzone_distance;
}
