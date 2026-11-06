# main/ is the application shell: it starts the status LED and the bridge, nothing else.
set(dependencies "status_led" "bridge")

idf_component_register(SRCS "main.c"
                       REQUIRES ${dependencies}
                       INCLUDE_DIRS ".")
