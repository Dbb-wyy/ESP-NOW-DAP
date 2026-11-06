set(srcs
    "main.c"
    "bridge_glue.c"
)

# usb_device is present on every target: on C3/C6 it contributes no sources and
# nothing in main/ references its API (those builds are the wireless slave).
set(dependencies "esp_timer" "status_led" "target_uart" "debug_probe" "usb_device" "esp_ringbuf")

if(NOT CONFIG_BRIDGE_ROLE_WIRED)
    list(APPEND dependencies "wireless_link")
endif()

# Starting from esp-idf v5.3, the GPIO driver is moved to a separate component
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    list(APPEND dependencies "esp_driver_gpio")
else()
    list(APPEND dependencies "driver")
endif()

idf_component_register(SRCS ${srcs}
                       REQUIRES ${dependencies}
                       INCLUDE_DIRS ".")
