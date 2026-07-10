#
# Copyright 2015, Broadcom Corporation
# All Rights Reserved.
#
# This is UNPUBLISHED PROPRIETARY SOURCE CODE of Broadcom Corporation;
# the contents of this file may not be disclosed to third parties, copied
# or duplicated in any form, in whole or in part, without the prior
# written permission of Broadcom Corporation.
#

NAME := Platform_AWAIR

WLAN_CHIP            := 43362
WLAN_CHIP_REVISION   := A2
HOST_MCU_FAMILY      := STM32F4xx
HOST_MCU_VARIANT     := STM32F411
HOST_MCU_PART_NUMBER := STM32F411CE

INTERNAL_MEMORY_RESOURCES = $(ALL_RESOURCES)

ifndef BUS
BUS := SDIO
endif

EXTRA_TARGET_MAKEFILES +=  $(MAKEFILES_PATH)/standard_platform_targets.mk

VALID_BUSES := SDIO SPI

# Keep the 43362 firmware in the EMW3165's external 2 MiB SPI flash instead
# of spending roughly 205 KiB of the STM32F411's 512 KiB internal flash on
# a byte-for-byte copy.  The app lookup table and firmware sector are
# provisioned by scripts/flash_wifi_firmware_sflash.zsh.
ifeq ($(NO_WIFI_FIRMWARE),)
MULTI_APP_WIFI_FIRMWARE := resources/firmware/$(WLAN_CHIP)/$(WLAN_CHIP)$(WLAN_CHIP_REVISION)$(WLAN_CHIP_BIN_TYPE).bin
endif

ifeq ($(BUS),SDIO)
ifeq ($(MULTI_APP_WIFI_FIRMWARE),)
GLOBAL_DEFINES          += WWD_DIRECT_RESOURCES
else
# Setting some internal build parameters
WIFI_FIRMWARE           := $(MULTI_APP_WIFI_FIRMWARE)
WIFI_FIRMWARE_LOCATION 	:= WIFI_FIRMWARE_IN_MULTI_APP
GLOBAL_DEFINES          += WIFI_FIRMWARE_IN_MULTI_APP
endif
endif
# Global includes
GLOBAL_INCLUDES  := .

# Global defines
# HSE_VALUE = STM32 crystal frequency = 26MHz (needed to make UART work correctly)
GLOBAL_DEFINES += HSE_VALUE=26000000
# Keep SysTick running in idle. The WICED 3.3.1 tickless hook can remain in
# platform_power_down_hook instead of waking after short sleeps on this board,
# eventually tripping the hardware watchdog. WICED_DISABLE_MCU_POWERSAVE still
# uses the ordinary WFI idle hook; it only disables the fragile tickless path.
GLOBAL_DEFINES += WICED_DISABLE_MCU_POWERSAVE
GLOBAL_DEFINES += $$(if $$(NO_CRLF_STDIO_REPLACEMENT),,CRLF_STDIO_REPLACEMENT)

# Components
$(NAME)_COMPONENTS += drivers/spi_flash

# Source files
$(NAME)_SOURCES := platform.c \
                   rewair_ota_common.c

ifeq ($(APP),bootloader)
$(NAME)_SOURCES += rewair_ota_boot.c
endif

# WICED APPS
# APP0 and FILESYSTEM_IMAGE are reserved main app and resources file system
# FR_APP := resources/sflash/snip_ota_fr-BCM943362WCD6.stripped.elf
# DCT_IMAGE :=
# OTA_APP :=
# FILESYSTEM_IMAGE :=
# WIFI_FIRMWARE :=
# APP0 :=
# APP1 :=
# APP2 :=

# WICED apps lookup table and BCM43362A2 image. Sectors below 0x101 are
# owned by the custom OTA stage/backup/journal; 0x136 and above are free.
APPS_LUT_HEADER_LOC := 0x101000
APPS_START_SECTOR := 258

ifneq ($(APP),bootloader)
ifneq ($(MAIN_COMPONENT_PROCESSING),1)
$(info +-----------------------------------------------------------------------------------------------------+ )
$(info | IMPORTANT NOTES                                                                                     | )
$(info +-----------------------------------------------------------------------------------------------------+ )
$(info | Wi-Fi MAC Address                                                                                   | )
$(info |    The target Wi-Fi MAC address is defined in <WICED-SDK>/generated_mac_address.txt                 | )
$(info |    Ensure each target device has a unique address.                                                  | )
$(info +-----------------------------------------------------------------------------------------------------+ )
$(info | MCU & Wi-Fi Power Save                                                                              | )
$(info |    It is *critical* that applications using WICED Powersave API functions connect an accurate 32kHz | )
$(info |    reference clock to the sleep clock input pin of the WLAN chip. Please read the WICED Powersave   | )
$(info |    Application Note located in the documentation directory if you plan to use powersave features.   | )
$(info +-----------------------------------------------------------------------------------------------------+ )
endif
endif
