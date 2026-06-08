set pagination off
set confirm off
set print pretty on

target extended-remote :1337
interrupt

echo \n=== F103 core registers ===\n
info registers pc sp lr xpsr

echo \n=== useful known F103 code addresses ===\n
echo wifi_uart_task       0x08009fc4\n
echo read_frame_header    0x0800c0d0\n
echo dispatch_inbound     0x080082f8\n
echo in_NETW              0x0800842a\n
echo in_TIME              0x08008564\n
echo in_TINF              0x080086fc\n
echo in_DISP              0x08008ea8\n
echo in_TEST              0x080097a4\n
echo out_SENS             0x080099dc\n
echo write_frame          0x0800c086\n

detach
quit
