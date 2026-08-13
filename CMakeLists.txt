cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# esp_codec_dev's legacy-I2C path (CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE)
# includes driver/i2c.h, which IDF v6 moved out of the always-available set --
# the same split that broke our own build in #14. The managed component's
# manifest predates v6 and doesn't request `driver`, and editing a managed
# component is futile since it's re-fetched.
#
# Making `driver` a common requirement satisfies it without touching the
# component. This is a stopgap: the real fix is migrating our I2C to
# driver/i2c_master.h (#30), after which the component's modern path is used
# and this line goes away.
idf_build_set_property(__COMPONENT_REQUIRES_COMMON driver APPEND)

project(tdeck-max-phone)
