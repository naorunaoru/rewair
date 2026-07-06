--------------------------------------------
BCM94332WCD4 - README
--------------------------------------------

Provider    : Broadcom
Website     : http://broadcom.com/wiced
Description : Broadcom BCM943362WCD4 mounted on a Broadcom BCM9WCD1EVAL1 evaluation board

Schematics & Photos : /platforms/BCM943362WCD4/schematics/
                      /platforms/evaluation_boards/BCM9WCD1EVAL1/

Module
  Mfr     : Broadcom
  P/N     : BCM943362WCD4
  MCU     : STM32F205RGT6
  WLAN    : USI WM-N-BM-02 SiP (BCM43362 Wi-Fi)
  Antenna : Diversity with two printed antennae (and in-line switched Murata MM8430 RF connectors)

EVB Features
  JLINK-JTAG debug interface
  USB-JTAG debug interface
  USB-serial UART interface
  Power supply : USB and/or external +5v
  Reset button
  Module current monitor
  Sensors/Peripherals
     - 2 x Buttons
     - 2 x LEDs
     - 1 x Thermistor
     - 1 x 8Mbit serial flash
  18-pin Expansion header

--------------------------------------------
Awair board notes (reverse-engineered)
--------------------------------------------

The actual Awair "The First Rewair" hardware differs from the reference
BCM943362WCD4 layout above for the external SPI flash:

  External SPI flash : Macronix MX25L1606E, 2 MiB, JEDEC ID c2 20 15
  SPI1 CS   (SSN)  : PA15  (WICED_GPIO_15) -- software GPIO, active low
  SPI1 SCK         : PB3   (WICED_GPIO_16), AF5 (GPIO_AF_SPI1)
  SPI1 MISO        : PB4   (WICED_GPIO_17), AF5 (GPIO_AF_SPI1)
  SPI1 MOSI        : PA7   (WICED_GPIO_8),  AF5 (GPIO_AF_SPI1)

PA15, PB3, and PB4 are also the module's JTAG TDI/TDO/NTRST pins. Repurposing
them for SPI1 is safe on this board because on-target debugging is SWD-only
(PA13/PA14 -- JTMS-SWDIO/JTCK-SWCLK); full JTAG (TDI/TDO/NTRST) is not used.

Chip select is driven as a plain push-pull GPIO by
WICED/platform/MCU/STM32F4xx/peripherals/platform_spi.c
(platform_spi_init/platform_spi_transfer), not by the SPI1 hardware NSS
line -- SPI_NSS_Soft is configured in that driver regardless of platform.
