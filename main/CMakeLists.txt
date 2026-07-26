idf_component_register(
    SRCS "main.c" "app_matter.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash dht11 thermostat_logic led_strip_control button_handler
)
