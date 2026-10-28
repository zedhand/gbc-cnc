# LinkBus CNC pendant — GBA multiboot ROM
# Needs devkitARM (devkitPro). `make` -> pendant_mb.gba, `make install` -> copies it into the
# ESP module's LittleFS data folder as rom/pendant.gba.
ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set. Install devkitPro (gba-dev) first: https://devkitpro.org/wiki/Getting_Started)
endif

TARGET   := pendant_mb
SHARED   := ../../shared/linkbus_proto
COMMON   := ../common
SOURCES  := src/main.c src/app.c src/ui.c $(COMMON)/lb_gba.c $(COMMON)/gfx.c $(COMMON)/font8x8.c \
            $(SHARED)/linkbus_proto.c
PREFIX   := $(DEVKITARM)/bin/arm-none-eabi-
CC       := $(PREFIX)gcc
OBJCOPY  := $(PREFIX)objcopy
GBAFIX   := $(DEVKITPRO)/tools/bin/gbafix

ARCH     := -mthumb -mthumb-interwork -mcpu=arm7tdmi -mtune=arm7tdmi
CFLAGS   := $(ARCH) -O2 -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections \
            -fno-strict-aliasing -I$(COMMON) -I$(SHARED) -Isrc
LDFLAGS  := $(ARCH) -specs=gba_mb.specs -Wl,--gc-sections -Wl,-Map,$(TARGET).map

BUILD    := build
OBJS     := $(addprefix $(BUILD)/,$(notdir $(SOURCES:.c=.o)))
vpath %.c src $(COMMON) $(SHARED)

all: $(TARGET).gba

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

$(TARGET).gba: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -t"LINKBUS CNC" -cLBCN -m01
	@echo "built $@ ($$(stat -c%s $@ 2>/dev/null || stat -f%z $@) bytes)"

install: $(TARGET).gba
	mkdir -p ../../esp/data/rom
	cp $(TARGET).gba ../../esp/data/rom/pendant.gba
	@echo "now: cd ../../esp && pio run -t uploadfs  (or upload the .gba in the web UI)"

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).gba $(TARGET).map

.PHONY: all install clean
