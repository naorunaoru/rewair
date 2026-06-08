/*
 * Local bridge factory Wi-Fi DCT defaults.
 *
 * Runtime joins save credentials into the standard WICED stored_ap_list[0].
 * The factory client AP is intentionally empty so reflashing the DCT never
 * makes the module chase SDK sample credentials or stale owner networks.
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#define CONFIG_AP_SSID       "WICED Config"
#define CONFIG_AP_CHANNEL    1
#define CONFIG_AP_SECURITY   WICED_SECURITY_WPA2_AES_PSK
#define CONFIG_AP_PASSPHRASE "12345678"

#define SOFT_AP_SSID         "WICED Device"
#define SOFT_AP_CHANNEL      1
#define SOFT_AP_SECURITY     WICED_SECURITY_WPA2_AES_PSK
#define SOFT_AP_PASSPHRASE   "WICED_PASSPHRASE"

#define CLIENT_AP_SSID       ""
#define CLIENT_AP_PASSPHRASE ""
#define CLIENT_AP_BSS_TYPE   WICED_BSS_TYPE_INFRASTRUCTURE
#define CLIENT_AP_SECURITY   WICED_SECURITY_WPA2_AES_PSK
#define CLIENT_AP_CHANNEL    0
#define CLIENT_AP_BAND       WICED_802_11_BAND_2_4GHZ

#define WICED_NETWORK_INTERFACE WICED_STA_INTERFACE

#ifdef __cplusplus
} /* extern "C" */
#endif
