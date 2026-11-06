# The bridge owns the role logic: shared pieces in bridge.c, one file per role.
# All four files compile on every target; the role guards inside them decide what
# is code and what is an empty translation unit.
idf_component_register(SRCS "bridge.c" "bridge_wired.c" "bridge_host.c" "bridge_slave.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES "freertos" "esp_timer" "esp_ringbuf" "esp_hw_support"
                                     "target_uart" "debug_probe" "status_led"
                                     "usb_device" "wireless_link")
