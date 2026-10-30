set(dependencies
    "esp_wifi"
    "esp_netif"
    "esp_event"
    "nvs_flash"
    "esp_timer"
    "esp_ringbuf"
)

if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    list(APPEND dependencies "esp_driver_gpio")
else()
    list(APPEND dependencies "driver")
endif()

idf_component_register(SRCS "wireless_link.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ${dependencies}
                      )
