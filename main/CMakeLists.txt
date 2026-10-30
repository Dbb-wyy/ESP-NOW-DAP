set(srcs
    "main.c"
    "bridge_glue.c"
)

set(dependencies "esp_timer" "util" "serial_handler" "debug_probe" "esp_ringbuf")

# The USB device stack (tinyusb + vendor bulk + CDC) only exists on the PC-facing
# roles, wired and wireless host. The wireless slave has no USB at all, which is
# what lets it build for targets without a USB OTG controller (ESP32-C3/C6).
if(NOT CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE)
    list(APPEND srcs "eub_vendord.c" "serial_bridge.c" "usb_phy.c")
    list(APPEND dependencies "tinyusb" "usb")
endif()

if(CONFIG_BRIDGE_ROLE_WIRED)
    # The mass storage / UF2 interface only exists in the wired build.
    list(APPEND srcs "msc.c")
else()
    list(APPEND dependencies "wireless_link")
endif()

# Starting from esp-idf v5.3, the GPIO driver is moved to a separate component
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    list(APPEND dependencies "esp_driver_gpio")
else()
    list(APPEND dependencies "driver")
endif()

# noflash.lf only places eub_vendord in IRAM; that object is not built for the slave.
set(ldfragments)
if(NOT CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE)
    set(ldfragments "noflash.lf")
endif()

idf_component_register(SRCS ${srcs}
                       REQUIRES ${dependencies}
                       LDFRAGMENTS ${ldfragments}
                       INCLUDE_DIRS "." "public_include")

# main/public_include/tusb_config.h is tinyusb's configuration header. tinyusb is
# pulled in by idf_component.yml for USB-capable targets only (S2/S3), and the
# component manager resolves that per TARGET, not per role: on S3 the component is
# part of every build, including the wireless slave one. Whenever it is present it
# must be able to find this header, so the include path is added regardless of the
# role. The slave build has no USB code of its own and nothing references tinyusb,
# so the library is built but never linked into the image.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("espressif__tinyusb" IN_LIST build_components)
    idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
    target_include_directories(${tusb_lib} PRIVATE public_include)
endif()

idf_build_get_property(project_ver PROJECT_VER)
if(project_ver)
    message(STATUS "Project version: '${project_ver}'")
    # Once a git tag is created, PROJECT_VER will look like v1.2-4-g567890a, assuming
    # that we use tags in `vX.Y` format. Extract X and Y from the tag:
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

target_compile_definitions(${COMPONENT_LIB} PRIVATE BCDDEVICE=${bcd_device})
