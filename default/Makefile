## General Flags
PROJECT = UzePSG
MCU = atmega644
TARGET = $(PROJECT).elf
CC = avr-gcc
INFO=../gameinfo.properties
UZEBIN_DIR = ../../../bin

## Kernel settings
KERNEL_DIR = ../../../kernel

KERNEL_OPTIONS = -DVIDEO_MODE=3 -DINTRO_LOGO=0 -DTILE_WIDTH=8 -DSCREEN_TILES_V=15 -DFIRST_RENDER_LINE=68
KERNEL_OPTIONS += -DMAX_SPRITES=1 -DRAM_TILES_COUNT=4 -DRESOLUTION_EXT=1 -DVRAM_TILES_H=32 ##-DSNES_MOUSE=1
KERNEL_OPTIONS += -DUART=2 -DUART_RX_BUFFER_SIZE=256 -DUART_TX_BUFFER_SIZE=8 -DINCLUDE_DEFAULT_WAVES=0
KERNEL_OPTIONS += -DSOUND_MIXER=0 -DENABLE_MIXER=0 -DCONTROLLERS_VSYNC_READ=0 -DTRANSLUCENT_COLOR=0xC7
KERNEL_OPTIONS += -DPF_USE_LSEEK=1 -DPF_USE_DIR=1 -DSPIRAM_DETECT_SIZE=1 ##-DPF_USE_WRITE=1

## Demo settings
##DEMO_OPTIONS = -DFORMAT_EEPROM=1

## Options common to compile, link and assembly rules
COMMON = -mmcu=$(MCU)

## Compile options common for all C compilation units.
CFLAGS = $(COMMON)
CFLAGS += -Wall -gdwarf-2 -std=gnu99 -DF_CPU=28636360UL -O2 -fsigned-char -ffunction-sections 
CFLAGS += -MD -MP -MT $(*F).o -MF dep/$(@F).d 
CFLAGS += $(KERNEL_OPTIONS)
CFLAGS += $(DEMO_OPTIONS)
CFLAGS += -mcall-prologues -fdata-sections -ffreestanding -fomit-frame-pointer

## Assembly specific flags
ASMFLAGS = $(COMMON)
ASMFLAGS += $(CFLAGS)
ASMFLAGS += -x assembler-with-cpp -Wa,-gdwarf2

## Linker flags
LDFLAGS = $(COMMON)
LDFLAGS += -Wl,-Map=$(PROJECT).map 
LDFLAGS += -Wl,-gc-sections 


## Intel Hex file production flags
HEX_FLASH_FLAGS = -R .eeprom

HEX_EEPROM_FLAGS = -j .eeprom
HEX_EEPROM_FLAGS += --set-section-flags=.eeprom="alloc,load"
HEX_EEPROM_FLAGS += --change-section-lma .eeprom=0 --no-change-warnings


## Objects that must be built in order to link
OBJECTS = uzeboxVideoEngineCore.o uzeboxCore.o uzeboxVideoEngine.o pff.o diskio.o spiram.o $(PROJECT).o soundMixerCustom.o

## Objects explicitly added by the user
LINKONLYOBJECTS = 

## Include Directories
INCLUDES = -I"$(KERNEL_DIR)" 

## Resources
RESOURCES = ../data/tiles.inc #../data/adpcm.inc

## Build
all: $(RESOURCES) $(TARGET) $(PROJECT).hex $(PROJECT).eep $(PROJECT).lss $(PROJECT).uze size

## Rebuild graphics resources
../data/tiles.inc: ../data/tiles.png ../data/tiles.xml
	$(UZEBIN_DIR)/gconvert ../data/tiles.xml

#../data/adpcm.inc: ../data/UzePSG.raw
#	$(UZEBIN_DIR)/dconvert ../data/adpcm.cfg

## Compile Kernel files
uzeboxVideoEngineCore.o: $(KERNEL_DIR)/uzeboxVideoEngineCore.s
	$(CC) $(INCLUDES) $(ASMFLAGS) -c  $<

uzeboxCore.o: $(KERNEL_DIR)/uzeboxCore.c
	$(CC) $(INCLUDES) $(CFLAGS) -c  $<

soundMixerCustom.o: ../soundMixerCustom.s
	$(CC) $(INCLUDES) $(ASMFLAGS) -c  $<

uzeboxVideoEngine.o: $(KERNEL_DIR)/uzeboxVideoEngine.c
	$(CC) $(INCLUDES) $(CFLAGS) -c  $<

diskio.o: $(KERNEL_DIR)/petitfatfs/diskio.c
	$(CC) $(INCLUDES) $(CFLAGS) -c  $<

pff.o: $(KERNEL_DIR)/petitfatfs/pff.c
	$(CC) $(INCLUDES) $(CFLAGS) -c  $<

spiram.o: $(KERNEL_DIR)/spiram.s $(DIRS)
	$(CC) $(INCLUDES) $(ASMFLAGS) -c $< -o $@

## Compile sources
$(PROJECT).o: ../$(PROJECT).c
	$(CC) $(INCLUDES) $(CFLAGS) -c  $<

##Link
$(TARGET): $(OBJECTS)
	 $(CC) $(LDFLAGS) $(OBJECTS) $(LINKONLYOBJECTS) $(LIBDIRS) $(LIBS) -o $(TARGET)

%.hex: $(TARGET)
	avr-objcopy -O ihex $(HEX_FLASH_FLAGS)  $< $@

%.eep: $(TARGET)
	-avr-objcopy $(HEX_EEPROM_FLAGS) -O ihex $< $@ || exit 0

%.lss: $(TARGET)
	avr-objdump -h -S $< > $@

%.uze: $(TARGET)
	-$(UZEBIN_DIR)/packrom $(PROJECT).hex $@ $(INFO)

size: ${TARGET}
	@echo
	@avr-size -C --mcu=${MCU} ${TARGET}

## Clean target
.PHONY: clean
clean:
	-rm -rf $(RESOURCES) $(OBJECTS) $(PROJECT).* dep/* *.uze


## Other dependencies
-include $(shell mkdir dep 2>/dev/null) $(wildcard dep/*)

