idf_component_register(
    SRCS
        "main.c"
        "iq_decode.c"
        "wifi5.c"
        "rf_dump.c"
        "wbfm_direct.c"
        "parlio_direct.c"
        "realtime.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_phy
        esp_wifi
        esp_event
        esp_netif
        esp_driver_parlio
        esp_driver_bitscrambler
        heap
        esp_hal_security
        ulp
)

set(ulp_app_name c5vrx2_lp)
set(ulp_sources "lp_core/realtime_lp.c")
set(ulp_exp_dep_srcs "realtime.c")
ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")

target_bitscrambler_add_src("c5vrx2_wbfm_direct6_4to1.bsasm")

target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${CMAKE_CURRENT_LIST_DIR}/rf_dump_memory.ld")
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/rf_dump_memory.ld")
