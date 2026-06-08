CC := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
OBJDUMP := arm-none-eabi-objdump
SIZE := arm-none-eabi-size

CFLAGS := -mcpu=cortex-m4 -mthumb -Os -g3 -ffreestanding -fno-builtin \
	-fdata-sections -ffunction-sections -Wall -Wextra -Werror
LDFLAGS := -T linker_flash.ld -nostdlib -Wl,--gc-sections \
	-Wl,-Map=build/emw3165_sensor_console.map

BUILD := build
ELF := $(BUILD)/emw3165_sensor_console.elf
BIN := $(BUILD)/emw3165_sensor_console.bin

.PHONY: all clean disasm

all: $(ELF) $(BIN)
	$(SIZE) $(ELF)

$(BUILD):
	mkdir -p $(BUILD)

$(ELF): emw3165_sensor_console.c linker_flash.ld | $(BUILD)
	$(CC) $(CFLAGS) emw3165_sensor_console.c $(LDFLAGS) -o $@

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

disasm: $(ELF)
	$(OBJDUMP) -d -S $<

clean:
	rm -rf $(BUILD)
