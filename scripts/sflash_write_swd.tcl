#
# sflash_write_swd.tcl
#
# Modern (SWD / CMSIS-DAP / OpenOCD 0.12+) reimplementation of the WICED SDK's
# apps/waf/sflash_write/sflash_write.tcl driver protocol. The vendored SDK tcl
# assumes JTAG-only 2015-era OpenOCD (`jtag newtap`, `find mfg_spi_flash/...`,
# a Win32 `echo` helper) and does not work with a CMSIS-DAP probe over SWD or
# with brew's OpenOCD 0.12. This script speaks the exact same wire protocol to
# the same waf_sflash_write RAM stub (data_config_area_t / data_transfer_area_t
# at the start of SRAM -- see apps/waf/sflash_write/sflash_write.c and
# WICED/platform/MCU/STM32F4xx/GCC/app_ram.ld) but drives it with
# `dap create` / `cortex_m` targets set up by OpenOCD's own
# target/stm32f4x.cfg, which supports SWD natively.
#
# Usage (via openocd -f):
#   openocd -f interface/cmsis-dap.cfg -c "transport select swd" \
#           -f target/stm32f4x.cfg \
#           -f scripts/sflash_write_swd.tcl \
#           -c "sflash_stub_elf \"<path-to-waf_sflash_write.elf>\"" \
#           -c "sflash_write_file \"<image.bin>\" 0x1c0000" \
#           -c "shutdown"
#
# Reading back is left to the firmware's own console `sflash read` / the
# /api/debug/sflash HTTP route (Task 1) rather than duplicated here, since
# byte-perfect verification only matters over that channel per the Task 2
# acceptance criteria; sflash_read_file is provided anyway for completeness /
# scripted host-side verification without booting the app.
#

# RAM base for STM32F411 (WICED CHIP_RAM_START for this family).
set MemoryStart 0x20000000

# Layout must match data_config_area_t / data_transfer_area_t in
# apps/waf/sflash_write/sflash_write.c
set entry_address_loc  [expr { $MemoryStart + 0x00 }]
set stack_address_loc  [expr { $MemoryStart + 0x04 }]
set buffer_size_loc    [expr { $MemoryStart + 0x08 }]

set data_size_loc      [expr { $MemoryStart + 0x0C }]
set dest_address_loc   [expr { $MemoryStart + 0x10 }]
set command_loc        [expr { $MemoryStart + 0x14 }]
set result_loc         [expr { $MemoryStart + 0x18 }]
set data_loc           [expr { $MemoryStart + 0x1C }]

# Must match MFG_SPI_FLASH_COMMAND_* in sflash_write.c
set COMMAND_INITIAL_VERIFY        0x01
set COMMAND_ERASE                 0x02
set COMMAND_WRITE                 0x04
set COMMAND_POST_WRITE_VERIFY     0x08
set COMMAND_VERIFY_CHIP_ERASURE   0x10
set COMMAND_READ                  0x40
set COMMAND_WRITE_ERASE_IF_NEEDED 0x80

# Must match mfg_spi_flash_result_t in sflash_write.c
array set RESULT {
    4294967295 "In Progress"
    0          "OK"
    1          "Erase Failed"
    2          "Verify after write failed"
    3          "Size too big for buffer"
    4          "Size too big for chip"
    5          "DCT location not found"
    6          "Error during Write"
    7          "Error during Read"
}

set sflash_stub_elf_path ""
set sflash_entry_address 0
set sflash_stack_address 0
set sflash_buffer_size   0

proc sflash_stub_elf { path } {
    global sflash_stub_elf_path
    set sflash_stub_elf_path $path
}

proc sflash_memread32 { address } {
    mem2array memar 32 $address 1
    # mem2array returns a "0x..."-formatted string. OpenOCD's Tcl interpreter
    # is Jim Tcl, where `expr { "0x0" }` yields back "0x0" rather than a
    # normalized decimal ("0x0" == 0 numerically, but string-keyed array
    # lookups like RESULT($v) need the decimal form) -- format "%d" forces
    # the conversion Jim Tcl's expr won't do here.
    return [format "%d" $memar(0)]
}

# Loads the waf_sflash_write ELF into RAM, starts it, and reads back the
# entry/stack/buffer-size fields it publishes at the start of RAM so we know
# how to talk to it.
proc sflash_stub_init { } {
    global entry_address_loc stack_address_loc buffer_size_loc
    global sflash_stub_elf_path sflash_entry_address sflash_stack_address sflash_buffer_size

    if { $sflash_stub_elf_path == "" } {
        puts "Error: call sflash_stub_elf <path> before sflash_write_file / sflash_read_file"
        exit 1
    }

    init
    reset halt
    halt

    puts "Loading sflash_write stub: $sflash_stub_elf_path"
    load_image $sflash_stub_elf_path

    set sflash_entry_address [sflash_memread32 $entry_address_loc]
    set sflash_stack_address [sflash_memread32 $stack_address_loc]
    set sflash_buffer_size   [sflash_memread32 $buffer_size_loc]

    puts [format "entry_address = 0x%x" $sflash_entry_address]
    puts [format "stack_address = 0x%x" $sflash_stack_address]
    puts [format "buffer_size   = 0x%x" $sflash_buffer_size]

    if { $sflash_buffer_size == 0 } {
        puts "Error: buffer size read from target is zero -- stub did not load/start correctly"
        exit 1
    }

    reg pc $sflash_entry_address
    reg sp $sflash_stack_address
    resume
}

# Loads foffset..foffset+length of filename into RAM at $data_loc using the
# correct load_image invocation (binary format, explicit base-offset math so
# only the requested slice lands at data_loc).
proc sflash_stub_load_chunk { filename foffset length } {
    global data_loc
    load_image $filename [expr { $data_loc - $foffset }] bin $data_loc $length
}

# Executes one command against the running stub and blocks until it reports
# a result (or times out). Mirrors program_sflash in the SDK's tcl. filename
# may be "" for commands that don't transfer data from a file (e.g. READ,
# where the stub fills $data_loc itself and the caller dump_images it back).
proc sflash_stub_command { filename foffset data_size dest_addr cmd } {
    global data_size_loc data_loc dest_address_loc command_loc result_loc RESULT

    halt
    if { $data_size != 0 && $filename != "" } {
        sflash_stub_load_chunk $filename $foffset $data_size
    }

    mww $data_size_loc    $data_size
    mww $dest_address_loc $dest_addr
    mww $result_loc       0xffffffff
    mww $command_loc      $cmd
    resume

    set result_val 0xffffffff
    set loops 0
    while { $result_val == 0xffffffff && $loops < 200 } {
        sleep 100
        set result_val [sflash_memread32 $result_loc]
        incr loops
    }

    if { [info exists RESULT($result_val)] } {
        puts "Result: $RESULT($result_val)"
    } else {
        puts "Result: unknown code $result_val"
    }

    if { $result_val != 0 } {
        halt
        error "sflash_write stub command failed (code $result_val)"
    }
}

# Writes filename to the sflash at dest_address, erasing sectors as needed
# and verifying after write (WRITE_ERASE_IF_NEEDED | POST_WRITE_VERIFY, same
# combination the SDK's sflash_write_file uses).
proc sflash_write_file { filename dest_address } {
    global COMMAND_WRITE_ERASE_IF_NEEDED COMMAND_POST_WRITE_VERIFY sflash_buffer_size

    sflash_stub_init

    set write_cmd [expr { $COMMAND_WRITE_ERASE_IF_NEEDED | $COMMAND_POST_WRITE_VERIFY }]
    set total_size [file size $filename]
    set pos 0

    puts "Total write size is $total_size bytes to [format 0x%x $dest_address]"
    while { $pos < $total_size } {
        set remaining [expr { $total_size - $pos }]
        set chunk [expr { $remaining < $sflash_buffer_size ? $remaining : $sflash_buffer_size }]
        puts "writing $chunk bytes at [format 0x%x [expr { $dest_address + $pos }]]"
        sflash_stub_command $filename $pos $chunk [expr { $dest_address + $pos }] $write_cmd
        set pos [expr { $pos + $chunk }]
    }

    puts "sflash_write_file: done ($total_size bytes written to [format 0x%x $dest_address])"
}

# Reads length bytes from src_address into filename. Provided for host-side
# scripted verification; the firmware's own read path (console/HTTP route)
# remains the acceptance-test channel.
proc sflash_read_file { filename src_address length } {
    global COMMAND_READ data_loc sflash_buffer_size

    sflash_stub_init

    set out [open $filename w]
    close $out

    set pos 0
    puts "Total read size is $length bytes from [format 0x%x $src_address]"
    while { $pos < $length } {
        set remaining [expr { $length - $pos }]
        set chunk [expr { $remaining < $sflash_buffer_size ? $remaining : $sflash_buffer_size }]
        puts "reading $chunk bytes from [format 0x%x [expr { $src_address + $pos }]]"
        sflash_stub_command "" 0 $chunk [expr { $src_address + $pos }] $COMMAND_READ
        halt
        dump_image "/tmp/sflash_read_chunk.bin" $data_loc $chunk
        set in [open "/tmp/sflash_read_chunk.bin" rb]
        set data [read $in]
        close $in
        set out [open $filename ab]
        fconfigure $out -translation binary
        puts -nonewline $out $data
        close $out
        set pos [expr { $pos + $chunk }]
    }
    file delete -force "/tmp/sflash_read_chunk.bin"
    puts "sflash_read_file: done ($length bytes read from [format 0x%x $src_address])"
}
