NAME := App_Rewair_Local_Bridge

$(NAME)_SOURCES := local_bridge.c \
                   rewair_state.c \
                   rewair_drops.c \
                   rewair_tz.c \
                   rewair_settings.c \
                   rewair_json.c \
                   rewair_uifs.c \
                   jsmn.c \
                   web_api.c \
                   web_ui.c \
                   ../../../libraries/protocols/SNTP/sntp.c

$(NAME)_INCLUDES := ../../../libraries/protocols/SNTP

$(NAME)_COMPONENTS += daemons/HTTP_server

GLOBAL_DEFINES += JSMN_PARENT_LINKS

WIFI_CONFIG_DCT_H := wifi_config_dct.h

VALID_OSNS_COMBOS := FreeRTOS-LwIP
