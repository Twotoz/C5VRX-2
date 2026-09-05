idf_component_register(
    SRCS
        "main.c"
        "startup_trace.c"
        "calibration.c"
        "continuous_iq.c"
        "diagnostics.c"
        "iq_decode.c"
        "wifi5.c"
        "rf_dump.c"
        "wbfm_direct.c"
        "parlio_direct.c"
        "realtime.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS
        "${IDF_PATH}/components/esp_driver_parlio/src"
    REQUIRES
        nvs_flash
        esp_partition
        esp_phy
        esp_wifi
        esp_event
        esp_netif
        esp_driver_parlio
        esp_driver_gpio
        esp_driver_bitscrambler
        esp_driver_dma
        esp_mm
        esp_pm
        esp_timer
        heap
)

target_bitscrambler_add_src("c5vrx2_wbfm_direct6_4to1.bsasm")

target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${CMAKE_CURRENT_LIST_DIR}/rf_dump_memory.ld")
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/rf_dump_memory.ld")
