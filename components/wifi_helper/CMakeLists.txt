set(component_srcs "src/wifi_helper.cpp")

idf_component_register(SRCS "${component_srcs}"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ""
                       PRIV_REQUIRES "esp_wifi"
                       REQUIRES "")

