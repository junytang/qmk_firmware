SRC += gesture_mode.c
SOFT_I2C_DRIVER = no
ifeq ($(SOFT_I2C_DRIVER), yes)
    I2C_DRIVER_REQUIRED = no
    SRC += soft_i2c.c
    OPT_DEFS += -DUSE_SOFT_I2C
else
    I2C_DRIVER_REQUIRED = yes
endif

TAP_DANCE_ENABLE = yes
