/* Software (bit-bang) I2C driver -- polled, no completion IRQ, hang-free.
 *
 * Why this exists: the RP2040 hardware I2C can busy-wait forever when the bus
 * gets stuck (e.g. the TMAG5273 holds SDA low after a glitch). That call lives
 * in housekeeping_task_user (the main loop), so a stuck bus freezes the entire
 * firmware (all keys dead, PMW3360 LED off) until a power cycle.
 *
 * This driver is fully polled (no interrupt can preempt the PMW3360 SPI read)
 * and every wait is bounded by a timeout, with an explicit bus-recovery path
 * (clock SDA free by toggling SCL). It therefore cannot hang the main loop.
 *
 * Select it with SOFT_I2C_DRIVER = yes in rules.mk (see post_rules.mk).
 */
#pragma once

#include <stdint.h>
#include "i2c_master.h"

#ifndef SOFT_I2C_SDA_PIN
    #ifdef I2C1_SDA_PIN
        #define SOFT_I2C_SDA_PIN I2C1_SDA_PIN
    #else
        #error "SOFT_I2C_SDA_PIN is not defined. Define it (or I2C1_SDA_PIN) in your board config."
    #endif
#endif

#ifndef SOFT_I2C_SCL_PIN
    #ifdef I2C1_SCL_PIN
        #define SOFT_I2C_SCL_PIN I2C1_SCL_PIN
    #else
        #error "SOFT_I2C_SCL_PIN is not defined. Define it (or I2C1_SCL_PIN) in your board config."
    #endif
#endif

/* Half-period of the SCL clock, in microseconds. ~5us => ~100kHz. */
#ifndef SOFT_I2C_CLOCK_DELAY_US
    #define SOFT_I2C_CLOCK_DELAY_US 5
#endif

/* Max time (us) to wait for SCL to rise on each clock. This is the clock-
    stretching tolerance: it must be long enough that a TMAG5273 stretching
    during a conversion finishes instead of us aborting mid-transfer (which is
    what wedges the slave). Only past this do we declare the bus stuck. Keep it
    as small as the sensor's normal stretch allows: a longer value only makes a
    rare bus fault stall the main loop (and thus freeze the pointer) for longer.
    5000us comfortably exceeds the TMAG5273's normal stretch at 32x oversampling
    while bounding any fault to a few ms instead of tens of ms. */
#ifndef SOFT_I2C_TIMEOUT_US
    #define SOFT_I2C_TIMEOUT_US 5000
#endif

/* After a stuck bus is detected, stay off the bus for this many milliseconds
   so the main loop stays free to service other inputs (e.g. the PMW3360)
   instead of being monopolized by I2C timeouts. */
#ifndef SOFT_I2C_BACKOFF_MS
    #define SOFT_I2C_BACKOFF_MS 50
#endif

/* Max time (us) to wait for SCL to rise inside soft_i2c_recover()'s clock
    toggles. Kept short: recover() now only runs from the wheel driver's lazy
    path (after several failed reads, bus idle), so it must not block long. */
#ifndef SOFT_I2C_RECOVER_TIMEOUT_US
    #define SOFT_I2C_RECOVER_TIMEOUT_US 500
#endif

void     soft_i2c_init(void);
i2c_status_t soft_i2c_read_register(uint8_t devaddr, uint8_t regaddr, uint8_t *data, uint16_t length, uint16_t timeout);
i2c_status_t soft_i2c_write_register(uint8_t devaddr, uint8_t regaddr, const uint8_t *data, uint16_t length, uint16_t timeout);
i2c_status_t soft_i2c_ping_address(uint8_t address, uint16_t timeout);
void     soft_i2c_recover(void);
