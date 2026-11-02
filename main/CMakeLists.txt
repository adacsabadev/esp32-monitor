idf_component_register(SRCS "main.c" "hwinfo.c" "bench.c"
                       REQUIRES esp_http_server esp_wifi esp_netif esp_event nvs_flash
                                esp_driver_tsens esp_timer esp_hw_support esp_rom esp_system
                                esp_app_format spi_flash freertos heap ulp
                                bt esp_driver_gpio esp_driver_i2c esp_partition esp_psram
                                efuse bootloader_support app_update soc mbedtls esp_hal_security
                       EMBED_TXTFILES "index.html")

set_source_files_properties("bench.c" PROPERTIES COMPILE_OPTIONS "-O2")

# ULP-RISC-V coprocessor program (runs on its own core, alongside the Xtensa cores).
set(ulp_app_name ulp_${COMPONENT_NAME})
set(ulp_riscv_sources "ulp/main.c")
set(ulp_exp_dep_srcs "main.c")
ulp_embed_binary(${ulp_app_name} "${ulp_riscv_sources}" "${ulp_exp_dep_srcs}")
