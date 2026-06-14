set(component_srcs "src/mqtt_helper.cpp")

idf_component_register(SRCS "${component_srcs}"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ""
                       PRIV_REQUIRES "mqtt" "app_update" "esp_timer" "esp_wifi" "esp_netif"
                       REQUIRES "")

