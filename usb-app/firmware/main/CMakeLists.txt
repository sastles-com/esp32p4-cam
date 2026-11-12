idf_component_register(
    SRCS "usbapp_main.c" "tunnel.c"
    REQUIRES camcore esp_netif esp_event lwip esp_driver_uart
)
