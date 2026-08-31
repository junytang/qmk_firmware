VPATH += keyboards/junytang/common
SRC += ploopyco.c
SRC += tmag5273wheel.c
SRC += gesture_mode.c
# Software I2C (bit-bang) option. Set SOFT_I2C_DRIVER = yes in rules.mk to use
# the polled, hang-free driver instead of the RP2040 hardware I2C (which can
# freeze the main loop on a stuck bus). When enabled, the hardware I2C driver is
# disabled (so it won't also drive GP6/GP7) and soft_i2c.c is compiled in.
SOFT_I2C_DRIVER = no
ifeq ($(SOFT_I2C_DRIVER), yes)
    I2C_DRIVER_REQUIRED = no
    SRC += soft_i2c.c
    OPT_DEFS += -DUSE_SOFT_I2C
else
    I2C_DRIVER_REQUIRED = yes
endif
POINTING_DEVICE_DRIVER = pmw3360
RGBLIGHT_ENABLE = yes

POINTING_DEVICE_ENABLE = yes
VIA_ENABLE = yes
TAP_DANCE_ENABLE = yes
# USER_NAME = junytang

## Uncomment to get debug output (also see keyboard_post_init_user()
## in keymap.c for more things to uncomment.)
# CONSOLE_ENABLE = yes
