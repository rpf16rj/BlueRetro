set(cov_srcs
    "main.c"
    "adapter/adapter.c"
    "adapter/config.c"
    "adapter/gameid.c"
    "adapter/hid_parser.c"
    "adapter/kb_monitor.c"
    "adapter/macro.c"
    "adapter/mapping_quirks.c"
    "adapter/memory_card.c"
    "adapter/wired/npiso.c"
    "adapter/wired/cdi.c"
    "adapter/wired/saturn.c"
    "adapter/wired/pce.c"
    "adapter/wired/genesis.c"
    "adapter/wired/real.c"
    "adapter/wired/jag.c"
    "adapter/wired/pcfx.c"
    "adapter/wired/jvs.c"
    "adapter/wired/n64.c"
    "adapter/wired/dc.c"
    "adapter/wired/gc.c"
    "adapter/wired/ps.c"
    "adapter/wired/sea.c"
    "adapter/wired/wii.c"
    "adapter/wired/parallel_1p.c"
    "adapter/wired/parallel_2p.c"
    "adapter/wired/wired.c"
    "adapter/wireless/hid_generic.c"
    "adapter/wireless/ps3.c"
    "adapter/wireless/wii.c"
    "adapter/wireless/ps.c"
    "adapter/wireless/sw.c"
    "adapter/wireless/sw2.c"
    "adapter/wireless/wireless.c"
    "bluetooth/host.c"
    "bluetooth/hci.c"
    "bluetooth/l2cap.c"
    "bluetooth/sdp.c"
    "bluetooth/att.c"
    "bluetooth/att_cfg.c"
    "bluetooth/att_hid.c"
    "bluetooth/smp.c"
    "bluetooth/mon.c"
    "bluetooth/hidp/hidp.c"
    "bluetooth/hidp/generic.c"
    "bluetooth/hidp/ps3.c"
    "bluetooth/hidp/wii.c"
    "bluetooth/hidp/ps.c"
    "bluetooth/hidp/sw.c"
    "bluetooth/hidp/sw2.c"
    "system/manager.c"
    "tools/devcrypto.c"
    "tools/ps1_gameid.c"
)

set(srcs
    "adapter/adapter_debug.c"
    "bluetooth/debug.c"
    "system/delay.c"
    "system/fpga_config.c"
    "system/fs.c"
    "system/gpio.c"
    "system/intr.c"
    "system/led.c"
    "system/bare_metal_app_cpu.c"
    "system/core0_stall.c"
    "system/core0_stall_highint_hdl.S"
    "system/startup.c"
    "tools/stats.c"
    "wired/detect.c"
    "wired/wired_bare.c"
    "wired/wired_rtos.c"
    "wired/npiso_io.c"
    "wired/cdi_uart.c"
    "wired/pce_io.c"
    "wired/sega_io.c"
    "wired/nsi.c"
    "wired/maple.c"
    "wired/jvs_uart.c"
    "wired/parallel.c"
    "wired/pcfx_spi.c"
    "wired/ps_spi.c"
    "wired/real_spi.c"
    "wired/snes_spi.c"
    "wired/jag_io.c"
    "wired/sea_io.c"
    "wired/wii_i2c.c"
    "zephyr/atomic.S"
)

if(CONFIG_BLUERETRO_WEBUI)
    list(APPEND srcs
        "net/webui.c"
    )
    set(embed_files "net/index.html")
endif()

if(CONFIG_BLUERETRO_WS_CMDS)
    list(APPEND srcs
        "tests/ws_srv.c"
        "tests/cmds.c"
    )
endif()

if(CONFIG_BLUERETRO_COVERAGE)
    list(APPEND srcs
        "tests/coverage.c"
    )
endif()

idf_component_register(
    SRCS "${srcs}" "${cov_srcs}"
    PRIV_INCLUDE_DIRS "."
    LDFRAGMENTS linker.lf
    EMBED_FILES "${embed_files}"
    PRIV_REQUIRES esp_coex esp_wifi esp_http_server json
        esp_driver_gpio esp_driver_uart esp_driver_spi esp_driver_ledc
        esp_driver_i2s esp_driver_rmt esp_driver_pcnt esp_driver_gptimer
        esp_driver_i2c esp_driver_sdio esp_driver_sdmmc esp_driver_sdspi
        esp_driver_mcpwm esp_driver_parlio esp_driver_twai esp_driver_dac
        esp_driver_tsens esp_driver_touch_sens esp_driver_usb_serial_jtag
        spiffs nvs_flash fatfs app_update esp_timer esp_event esp_netif
        lwip vfs esp_common esp_ringbuf soc driver bt freertos queue_bss
        efuse esp_system esp_hw_support esp_rom esp_mm esp_psram heap log
        hal newlib pthread spi_flash esp-tls mbedtls http_parser
        tcp_transport wpa_supplicant wifi_provisioning protocomm
        protobuf-c mqtt espcoredump esp_gdbstub esp_hid usb console ulp
        unity cmock cxx sdmmc rt wear_levelling esp_local_ctrl
        esp_https_server esp_https_ota esp_lcd esp_adc esp_eth esp_phy
        esp_pm esp_security perfmon esp_vfs_console bootloader
        bootloader_support nvs_sec_provider esp_app_format idf_test
        xtensa app_trace ieee802154 openthread esp_vfs_console
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-address-of-packed-member)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-builtin-memset)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-builtin-memcpy)

if(CONFIG_BLUERETRO_COVERAGE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fprofile-info-section)

    set_source_files_properties(
        ${cov_srcs}
        PROPERTIES COMPILE_FLAGS --coverage
    )

    target_link_libraries(${COMPONENT_TARGET} gcov)
endif()

idf_build_set_property(COMPILE_DEFINITIONS "-DBLUERETRO" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "-DXT_INTEXC_HOOKS" APPEND)

target_link_libraries(${COMPONENT_TARGET} "-u ld_include_my_startup_file")
target_link_libraries(${COMPONENT_TARGET} "-u ld_include_core0_stall_highint_hdl")
