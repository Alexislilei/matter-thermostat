idf_component_register(
    SRCS "led_control.c"
    INCLUDE_DIRS "include"
    REQUIRES driver led_strip esp_timer thermostat_logic freertos
)
