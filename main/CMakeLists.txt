# IDF v6.0 split the `driver` umbrella into esp_driver_* components; v5.x still
# ships gpio/i2s/spi bundled in `driver`. `driver` itself still provides the
# legacy driver/i2c.h wrapper on both, so it stays in REQUIRES either way.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(DRIVER_REQUIRES driver esp_driver_gpio esp_driver_i2s esp_driver_spi esp_driver_ledc)
else()
    set(DRIVER_REQUIRES driver)
endif()

# TDeckMaxAudioAnchor.cpp (a stub direct-3CX client) is intentionally left
# out of SRCS: this PoC's architecture routes 3CX entirely through a
# drawbridge PBX instance, so this device never talks to 3CX directly. Kept
# in the tree as reference, not deleted, per #10.
idf_component_register(
    SRCS
        "src/app_main.cpp"
        "src/phone_app.cpp"
        "src/net_wifi.c"
        "src/xl9555.c"
        "src/pmu_sy6970.c"
        "src/es8311_audio.c"
        "src/tca8418_keypad.cpp"
        "src/touch_cst3530.c"
        "src/epaper_display.cpp"
        # LVGL shell, snapshot of tdeck-glopanel (see docs/UI_SHELL.md)
        "src/lvgl_glue.c"
        "src/scr_mgr.c"
        "src/app_nav.c"
        "src/wifi_wizard.c"
        "src/view_settings.c"
        "src/view_contacts.c"
        "src/contacts.c"
        "src/call_log.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        ${DRIVER_REQUIRES}
        nvs_flash
        esp_event
        esp_wifi
        esp_netif
        esp_timer
        esp_hw_support
        lwip
        lvgl
        tincan_uac       # tincan-core (pulls sip_core/tincan_media/tincan_codecs)
        tincan_media     # PlayoutBuffer for the audio task's jitter buffer
)
