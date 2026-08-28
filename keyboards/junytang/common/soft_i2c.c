/* Software (bit-bang) I2C driver -- see soft_i2c.h for rationale.
 * Polled GPIO with emulated open-drain: a line is released by making it an
 * input (the external pull-up then pulls it HIGH) and driven LOW by making it
 * an output and writing 0. This avoids the RP2040 PAL open-drain quirk and any
 * hardware I2C peripheral, so it can never be preempted by an I2C completion
 * IRQ and never hangs the main loop. Every wait is timeout-bounded and
 * recoverable. */
#include "soft_i2c.h"
#include "gpio.h"
#include "wait.h"
#include "timer.h"

/* --- emulated open-drain GPIO helpers --------------------------------------
 * Releasing a line = input (floating, pulled up externally). Driving LOW =
 * output + write 0. This is the canonical RP2040 software-I2C open drain and
 * does not depend on PAL_MODE_OUTPUT_OPENDRAIN (undefined on RP2040). */
static inline void sda_low(void)  { gpio_set_pin_output(SOFT_I2C_SDA_PIN); gpio_write_pin_low(SOFT_I2C_SDA_PIN); }
static inline void sda_high(void) { gpio_set_pin_input(SOFT_I2C_SDA_PIN); }
static inline void scl_low(void)  { gpio_set_pin_output(SOFT_I2C_SCL_PIN); gpio_write_pin_low(SOFT_I2C_SCL_PIN); }
static inline void scl_high(void) { gpio_set_pin_input(SOFT_I2C_SCL_PIN); }
static inline bool sda_read(void) { return gpio_read_pin(SOFT_I2C_SDA_PIN); }
static inline bool scl_read(void) { return gpio_read_pin(SOFT_I2C_SCL_PIN); }

static inline void i2c_delay(void) { wait_us(SOFT_I2C_CLOCK_DELAY_US); }

/* When the bus is detected stuck (SCL won't rise), we stop touching it and
   back off for a cooldown. This keeps the main loop free to service other
   inputs (notably the PMW3360): without it, a stuck bus would monopolize the
   loop with timeouts and starve the pointer read, letting motion accumulate
   into a sudden jump. */
static uint32_t soft_i2c_blocked_until = 0;

/* Wait for the slave to release SCL (clock stretching), bounded by timeout.
 * Returns false on timeout so the caller can abort instead of hanging. */
static bool scl_wait_high(void) {
    uint16_t timeout = SOFT_I2C_TIMEOUT_US;
    while (!scl_read()) {
        if (--timeout == 0) {
            /* Bus is stuck: try to free it (called BEFORE arming the backoff
               so this attempt actually runs), then back off so we stop
               monopolizing the main loop. */
            soft_i2c_recover();
            soft_i2c_blocked_until = timer_read32() + SOFT_I2C_BACKOFF_MS;
            return false;
        }
        wait_us(1); // 每次等 1 微秒，精确控制微秒级超时
    }
    return true;
}

void soft_i2c_init(void) {
    /* Idle bus: both lines released (input) so the external pull-ups hold
       them HIGH. */
    gpio_set_pin_input(SOFT_I2C_SDA_PIN);
    gpio_set_pin_input(SOFT_I2C_SCL_PIN);
    wait_us(100);
}

/* --- primitives ------------------------------------------------------------ */

static void soft_i2c_start(void) {
    sda_high();
    scl_high();
    i2c_delay();
    sda_low(); /* SDA falls while SCL is HIGH = START */
    i2c_delay();
    scl_low();
    i2c_delay();
}

static void soft_i2c_stop(void) {
    sda_low();
    scl_high();
    i2c_delay(); /* SCL is pulled up; no need to wait (and waiting here would
                    re-trigger recovery if the bus is already stuck). */
    sda_high(); /* SDA rises while SCL is HIGH = STOP */
    i2c_delay();
}

/* Write one byte; returns true if the slave ACKed. */
static bool soft_i2c_write_byte(uint8_t b) {
    for (int8_t i = 7; i >= 0; i--) {
        if (b & (1u << i)) sda_high();
        else               sda_low();
        scl_high();
        if (!scl_wait_high()) return false;
        i2c_delay();
        scl_low();
        i2c_delay();
    }
    /* Clock the ACK bit: slave pulls SDA LOW to ACK. */
    sda_high();
    scl_high();
    if (!scl_wait_high()) return false;
    i2c_delay();
    bool acked = !sda_read();
    scl_low();
    i2c_delay();
    return acked;
}

/* Read one byte into *out; send ack (true) or nack (false) afterwards.
   Returns false on timeout so the caller can abort instead of returning
   garbage as "success". */
static bool soft_i2c_read_byte(bool ack, uint8_t *out) {
    uint8_t b = 0;
    sda_high(); /* release SDA for the slave to drive */
    for (int8_t i = 7; i >= 0; i--) {
        scl_high();
        if (!scl_wait_high()) return false;
        i2c_delay();
        b = (uint8_t)((b << 1) | (sda_read() ? 1u : 0u));
        scl_low();
        i2c_delay();
    }
    if (ack) sda_low();
    else     sda_high();
    scl_high();
    if (!scl_wait_high()) return false;
    i2c_delay();
    scl_low();
    i2c_delay();
    *out = b;
    return true;
}

/* --- high-level API (matches QMK i2c_master signature) -------------------- */

/* devaddr is expected to be pre-shifted (addr << 1), per QMK convention. */
i2c_status_t soft_i2c_write_register(uint8_t devaddr, uint8_t regaddr, const uint8_t *data, uint16_t length, uint16_t timeout) {
    (void)timeout;
    if (timer_read32() < soft_i2c_blocked_until) return I2C_STATUS_TIMEOUT;
    soft_i2c_start();
    if (!soft_i2c_write_byte((uint8_t)(devaddr & 0xFEu))) { /* write */
        soft_i2c_stop();
        return I2C_STATUS_ERROR;
    }
    if (!soft_i2c_write_byte(regaddr)) {
        soft_i2c_stop();
        return I2C_STATUS_ERROR;
    }
    for (uint16_t i = 0; i < length; i++) {
        if (!soft_i2c_write_byte(data[i])) {
            soft_i2c_stop();
            return I2C_STATUS_ERROR;
        }
    }
    soft_i2c_stop();
    return I2C_STATUS_SUCCESS;
}

i2c_status_t soft_i2c_read_register(uint8_t devaddr, uint8_t regaddr, uint8_t *data, uint16_t length, uint16_t timeout) {
    (void)timeout;
    if (timer_read32() < soft_i2c_blocked_until) return I2C_STATUS_TIMEOUT;
    soft_i2c_start();
    if (!soft_i2c_write_byte((uint8_t)(devaddr & 0xFEu))) { /* write reg pointer */
        soft_i2c_stop();
        return I2C_STATUS_ERROR;
    }
    if (!soft_i2c_write_byte(regaddr)) {
        soft_i2c_stop();
        return I2C_STATUS_ERROR;
    }
    soft_i2c_start();                                       /* repeated START */
    if (!soft_i2c_write_byte((uint8_t)(devaddr | 0x01u))) { /* read */
        soft_i2c_stop();
        return I2C_STATUS_ERROR;
    }
    for (uint16_t i = 0; i < length; i++) {
        /* ACK every byte except the last, which gets NACK. */
        uint8_t b;
        if (!soft_i2c_read_byte(i < (length - 1u), &b)) {
            soft_i2c_stop();
            return I2C_STATUS_ERROR;
        }
        data[i] = b;
    }
    soft_i2c_stop();
    return I2C_STATUS_SUCCESS;
}

i2c_status_t soft_i2c_ping_address(uint8_t address, uint16_t timeout) {
    if (timer_read32() < soft_i2c_blocked_until) return I2C_STATUS_TIMEOUT;
    uint8_t dummy = 0;
    return soft_i2c_read_register(address, 0x00, &dummy, sizeof(dummy), timeout);
}

/* Clock SDA free by toggling SCL up to 9 times, then issue a STOP.
 * Call this when a transaction has failed, to recover a stuck bus. */
void soft_i2c_recover(void) {
    /* If we're already backing off from a stuck bus, don't re-block the loop
       here; the cooldown already covers it. */
    if (timer_read32() < soft_i2c_blocked_until) return;
    sda_high();
    for (int i = 0; i < 9; i++) {
        scl_low();
        i2c_delay();
        scl_high();
        uint16_t timeout = SOFT_I2C_RECOVER_TIMEOUT_US;
        while (!scl_read()) {
            if (--timeout == 0) break;
            wait_us(1); // 每次等 1 微秒，精确控制微秒级超时
        }
        i2c_delay();
    }
    /* STOP */
    sda_low();
    scl_high();
    uint16_t timeout = SOFT_I2C_RECOVER_TIMEOUT_US;
    while (!scl_read()) {
        if (--timeout == 0) break;
        wait_us(1); // 每次等 1 微秒，精确控制微秒级超时
    }
    i2c_delay();
    sda_high();
    i2c_delay();
}
