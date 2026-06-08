NAME := App_Rewair_Local_Bridge

$(NAME)_SOURCES := local_bridge.c \
                   ../../../libraries/protocols/SNTP/sntp.c

$(NAME)_INCLUDES := ../../../libraries/protocols/SNTP

WIFI_CONFIG_DCT_H := wifi_config_dct.h

VALID_OSNS_COMBOS := FreeRTOS-LwIP
