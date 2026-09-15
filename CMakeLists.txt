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

# Shared endpoint core (sip_core parser, TincanUac, PlayoutBuffer/rtp, G.711)
# lives in the sibling tincan-core repo. Default expects a sibling checkout;
# CI or a nonstandard layout sets TINCAN_CORE_PATH.
if(DEFINED ENV{TINCAN_CORE_PATH})
    set(TINCAN_CORE_PATH $ENV{TINCAN_CORE_PATH})
else()
    get_filename_component(TINCAN_CORE_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/../tincan-core" ABSOLUTE)
endif()
if(NOT EXISTS "${TINCAN_CORE_PATH}/components/tincan_uac/CMakeLists.txt")
    message(FATAL_ERROR "tincan-core not found at ${TINCAN_CORE_PATH} -- "
        "clone github.com/GlomarGadaffi/tincan-core alongside this repo or "
        "set TINCAN_CORE_PATH")
endif()

# Advisory-only pin check: TINCAN_CORE_PIN records the tincan-core commit
# this tree was last verified against. A mismatch doesn't fail the build --
# it just means the sibling checkout hasn't been re-verified since it moved.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/TINCAN_CORE_PIN")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/TINCAN_CORE_PIN" TINCAN_CORE_PIN_LINES
        REGEX "^[^#]")
    list(GET TINCAN_CORE_PIN_LINES 0 TINCAN_CORE_PINNED_COMMIT)
    execute_process(
        COMMAND git -C "${TINCAN_CORE_PATH}" rev-parse HEAD
        OUTPUT_VARIABLE TINCAN_CORE_ACTUAL_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT TINCAN_CORE_ACTUAL_COMMIT STREQUAL TINCAN_CORE_PINNED_COMMIT)
        message(WARNING "tincan-core at ${TINCAN_CORE_PATH} is at "
            "${TINCAN_CORE_ACTUAL_COMMIT} but this build was last verified "
            "against ${TINCAN_CORE_PINNED_COMMIT} (see TINCAN_CORE_PIN)")
    endif()
endif()

set(EXTRA_COMPONENT_DIRS "${TINCAN_CORE_PATH}/components")

project(tdeck-max-phone)
