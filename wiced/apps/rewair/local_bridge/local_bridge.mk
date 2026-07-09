NAME := App_Rewair_Local_Bridge

$(NAME)_SOURCES := local_bridge.c \
                   rewair_state.c \
                   rewair_drops.c \
                   rewair_tz.c \
                   rewair_settings.c \
                   rewair_json.c \
                   rewair_uifs.c \
                   rewair_fmt.c \
                   rewair_walltime.c \
                   rewair_score.c \
                   rewair_frame_rx.c \
                   rewair_frames.c \
                   rewair_wifi_dct.c \
                   rewair_wifi_scan.c \
                   rewair_wifi_join.c \
                   rewair_net_mode.c \
                   rewair_ota.c \
                   rewair_console.c \
                   jsmn.c \
                   web_api.c \
                   web_ui.c \
                   ../../../libraries/protocols/SNTP/sntp.c

$(NAME)_INCLUDES := ../../../libraries/protocols/SNTP

$(NAME)_COMPONENTS += daemons/HTTP_server
$(NAME)_COMPONENTS += daemons/DNS_redirect

GLOBAL_DEFINES += JSMN_PARENT_LINKS

WIFI_CONFIG_DCT_H := wifi_config_dct.h

VALID_OSNS_COMBOS := FreeRTOS-LwIP
