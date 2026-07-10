NAME := App_Rewair_BI201_Probe

$(NAME)_SOURCES := bi201_probe.c

# USART1 is the BI201 transport.  Initializing it as stdio first would make the
# generic UART API reject it and would also leak diagnostics into the module.
GLOBAL_DEFINES += WICED_DISABLE_STDIO

VALID_OSNS_COMBOS := FreeRTOS-LwIP
