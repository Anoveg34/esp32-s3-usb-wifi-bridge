# RNDIS: custom descriptors + usb/net_class.c (esp_tinyusb does not compile tinyusb_net.c).
# NCM: default "USB net" descriptors + esp_tinyusb tinyusb_net.c (the path that Windows UsbNcm works with).
set(USB_SRCS)
if(CONFIG_EXAMPLE_USB_NET_MODE_NCM)
    list(APPEND USB_SRCS "usb/ncm/ncm_desc.c")
else()
    list(APPEND USB_SRCS "usb/net_class.c" "usb/rndis/rndis_desc.c")
endif()

idf_component_register(SRCS "main.c"
                            "config/app_config.c"
                            "boot/app_boot.c"
                            "wifi/wifi_ap.c"
                            "net/usb_wan.c"
                            "net/nat.c"
                            "web/web_server.c"
                            ${USB_SRCS}
                    INCLUDE_DIRS "." "config" "boot" "wifi" "net" "usb" "web" "usb/tusb_cfg"
                    EMBED_TXTFILES "web/www/index.html"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif esp_http_server lwip
                                  app_update espressif__esp_tinyusb espressif__tinyusb
                                  espressif__cjson espressif__mdns)

idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/usb/tusb_cfg")
idf_component_get_property(esp_tusb_lib espressif__esp_tinyusb COMPONENT_LIB)
target_include_directories(${esp_tusb_lib} BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/usb/tusb_cfg")

if(NOT CONFIG_EXAMPLE_USB_NET_MODE_NCM)
    idf_component_get_property(esp_tusb_lib espressif__esp_tinyusb COMPONENT_LIB)
    idf_component_get_property(esp_tusb_dir espressif__esp_tinyusb COMPONENT_DIR)
    set_source_files_properties("${esp_tusb_dir}/tinyusb_net.c"
                                TARGET_DIRECTORY ${esp_tusb_lib}
                                PROPERTIES HEADER_FILE_ONLY TRUE)
endif()
