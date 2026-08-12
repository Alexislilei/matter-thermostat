idf_component_register(
    SRCS "lcd_display.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd esp_lcd_ili9341 esp_lvgl_port esp_timer thermostat_logic
)