idf_component_register(
    SRCS "crash_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_system freertos nvs_flash
)
