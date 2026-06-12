# ============================================================================
#  src/CMakeLists.txt — main 组件
#  Arduino+ESP-IDF 双框架: PRIV_REQUIRES 列出我们直接 #include 的 ESP-IDF 组件
#  + 4G USB Host 三件套 (iot_usbh_rndis/cdc + iot_eth, 来自 managed_components/)
# ============================================================================
## main 组件 (v4.0)
## - SRCS: 主代码
## - PRIV_REQUIRES: 直接 #include 的 ESP-IDF 组件
## - USB CDC 通过 idf_component.yml 拉 espressif/iot_usbh_cdc
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_event
        esp_netif
        esp_http_client
        esp_wifi
        esp-tls
        lwip
        nvs_flash
        app_update
        espressif__iot_usbh_cdc
)
