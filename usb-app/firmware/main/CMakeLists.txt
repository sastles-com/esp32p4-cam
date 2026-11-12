idf_component_register(
    SRCS "usbapp_main.c"
    REQUIRES camcore esp_netif esp_event lwip esp_timer
)
