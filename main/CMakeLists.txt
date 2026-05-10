idf_component_register(
    SRCS "../microphone/microphone.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi nvs_flash lwip esp_netif esp_event
)