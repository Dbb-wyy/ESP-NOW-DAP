set(dependencies freertos esp_system esp_timer led_strip)

# RMT_CLK_SRC_DEFAULT and the RMT types come from the RMT driver; it moved to its own
# component in esp-idf v5.3
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    list(APPEND dependencies "esp_driver_rmt")
else()
    list(APPEND dependencies "driver")
endif()

idf_component_register(SRCS "status_led.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ${dependencies}
                      )
