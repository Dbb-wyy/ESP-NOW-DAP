# USB device stack (tinyusb + CDC + vendor bulk). Only USB-capable targets compile
# it: the wireless slave roles build on targets without a USB OTG controller
# (ESP32-C3/C6), where main/ still lists this component in REQUIRES but nothing is
# registered here and nothing references usb_device_start().
#
# Which sources exist is decided by the target, which is the only variable visible
# during component registration (CONFIG_* values are not available to the
# requirement pass).
set(usb_capable FALSE)
if(IDF_TARGET STREQUAL "esp32s2" OR IDF_TARGET STREQUAL "esp32s3")
    set(usb_capable TRUE)
endif()

set(srcs)
set(priv_requires "freertos")
set(ldfragments)

if(usb_capable)
    set(srcs
        "usb_device.c"
        "eub_vendord.c"
        "serial_bridge.c"
        "usb_phy.c"
        "eub_abort.c"
    )
    list(APPEND priv_requires "esp_timer" "tinyusb" "usb" "esp_ringbuf" "status_led")

    # Starting from esp-idf v5.3, the GPIO driver is moved to a separate component
    if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
        list(APPEND priv_requires "esp_driver_gpio")
    else()
        list(APPEND priv_requires "driver")
    endif()

    # noflash.lf only places eub_vendord in IRAM.
    set(ldfragments "noflash.lf")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ${priv_requires}
                       LDFRAGMENTS ${ldfragments})

# tusb_config.h is tinyusb's configuration header. tinyusb is resolved by the
# component manager for USB-capable targets only; when it is part of the build the
# library must be able to find this header.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("espressif__tinyusb" IN_LIST build_components)
    idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
    target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
endif()

# bcdDevice is derived from the git-described project version (tag format vX.Y).
# Once a tag exists PROJECT_VER looks like v1.2-4-g567890a.
idf_build_get_property(project_ver PROJECT_VER)
if(project_ver)
    message(STATUS "Project version: '${project_ver}'")
    string(REGEX MATCH "^v([0-9]+)\\.([0-9]+)[^.]*$" match ${project_ver})
    if(match)
        set(project_ver_major ${CMAKE_MATCH_1})
        set(project_ver_minor ${CMAKE_MATCH_2})
        # extend project_ver_minor to two BCD digits
        string(LENGTH ${project_ver_minor} len)
        if(len EQUAL 1)
            set(project_ver_minor "0${project_ver_minor}")
        endif()
        set(bcd_device "0x${project_ver_major}${project_ver_minor}")
    else()
        message(STATUS "Failed to determine major and minor version from '${project_ver}'")
    endif()
endif()

# Otherwise, set the version to 0.01
if(NOT bcd_device)
    set(bcd_device "0x0001")
endif()
message(STATUS "bcdDevice value: ${bcd_device}")

if(usb_capable)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BCDDEVICE=${bcd_device})
    # tusb_config.h sits in the component root (tinyusb includes it from tusb.h while
    # compiling this component's own sources). Keep that path private so it does not
    # leak into the include directories of dependents.
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
endif()
