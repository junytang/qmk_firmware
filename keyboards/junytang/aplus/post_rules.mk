VPATH += keyboards/junytang/common
SRC += ploopyco.c
SRC += tmag5273wheel.c
SRC += gesture_mode.c
I2C_DRIVER_REQUIRED = yes
POINTING_DEVICE_DRIVER = pmw3360
RGBLIGHT_ENABLE = yes

POINTING_DEVICE_ENABLE = yes
VIA_ENABLE = yes
TAP_DANCE_ENABLE = yes
# USER_NAME = junytang

## Uncomment to get debug output (also see keyboard_post_init_user()
## in keymap.c for more things to uncomment.)
#CONSOLE_ENABLE = yes
