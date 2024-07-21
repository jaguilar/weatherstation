add_pico_executable(weather main.cc)
target_link_libraries(weather PRIVATE common freertosxx pico_printf hardware_adc hardware_gpio hardware_watchdog etl::etl lwipxx_mqtt homeassistant FreeRTOS-Kernel)
target_compile_options(weather PRIVATE -DMQTT_HOST="$ENV{MQTT_HOST}" -DMQTT_USER="$ENV{MQTT_USER}" -DMQTT_PASSWORD="$ENV{MQTT_PASSWORD}" -DMQTT_CLIENT_ID="weatherstation")
pico_enable_stdio_uart(weather 1)