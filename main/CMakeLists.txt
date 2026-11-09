idf_component_register(
    SRCS
        "pentaxhub_main.c"
        "wifi_ap.c"
        "camera.c"
        "ptp.c"
        "ptp_transport.c"
        "bot.c"
        "usb_host_shared.c"
        "pentax_scsi.c"
        "pentax_status.c"
        "pentax_sd.c"
        "usb_msc_link.c"
        "photo_stream.c"
        "pentax_service.c"
        "pentax_http.c"
        "web_server.c"
        "ota_http.c"
    PRIV_REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        usb
        esp_http_server
        json
        fatfs
        app_update
    INCLUDE_DIRS "."
    EMBED_FILES "index.html" "app.js" "style.css"
        "pentax_index.html" "pentax_app.js" "pentax_style.css"
)
