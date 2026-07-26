idf_component_register(
    SRCS "button_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer thermostat_logic freertos
)
