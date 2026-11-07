idf_component_register(
    SRCS
        "pentaxhub_main.c"
        "wifi_ap.c"
        "camera.c"
        "ptp.c"
        "ptp_transport.c"
        "bot.c"
        "pentax_scsi.c"
        "usb_msc_link.c"
        "photo_stream.c"
        "pentax_service.c"
        "pentax_http.c"
        "web_server.c"
    PRIV_REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        usb
        esp_http_server
        json
    INCLUDE_DIRS "."
    EMBED_FILES "index.html" "app.js" "style.css"
)
