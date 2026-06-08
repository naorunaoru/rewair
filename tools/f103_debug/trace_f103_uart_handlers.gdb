set pagination off
set confirm off

target extended-remote :1337
interrupt

hbreak *0x0800842a
commands
  silent
  echo \n[hit] f103_in_NETW\n
  info registers pc lr r0 r1 r2 r3
  continue
end

hbreak *0x080086fc
commands
  silent
  echo \n[hit] f103_in_TINF\n
  info registers pc lr r0 r1 r2 r3
  continue
end

hbreak *0x08008564
commands
  silent
  echo \n[hit] f103_in_TIME\n
  info registers pc lr r0 r1 r2 r3
  continue
end

hbreak *0x08008ea8
commands
  silent
  echo \n[hit] f103_in_DISP\n
  info registers pc lr r0 r1 r2 r3
  continue
end

hbreak *0x080099dc
commands
  silent
  echo \n[hit] f103_out_SENS\n
  info registers pc lr r0 r1 r2 r3
  continue
end

continue
