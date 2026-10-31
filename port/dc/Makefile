# Klonoa: Empire of Dreams -- native Dreamcast build of the kleod sources.
#
# Usage (from the repository root, after building the GBA ROM once so kleod.elf exists):
#   source /path/to/kos/environ.sh
#   make -C port/dc
#
# The GBA build (root Makefile) is untouched. This Makefile only reads kleod's src/, include/,
# asm/data/ and the user's baserom.gba, and writes everything under port/dc/build/.

ROOT    := $(abspath ../..)
BUILD   := $(abspath build)
TARGET  := $(BUILD)/klonoa-dc.elf

ifeq ($(KOS_BASE),)
$(error KOS_BASE is not set: source your KallistiOS environ.sh first)
endif

CC      := $(KOS_CC)
AS      := $(KOS_AS)
OBJCOPY := $(KOS_OBJCOPY)
PREPROC := $(ROOT)/tools/preproc/preproc

# Game sources compiled unmodified. eeprom.c is replaced by hal_eeprom.c (it bit-bangs the
# cartridge EEPROM over DMA3); everything else is the original game logic.
GAME_SRCS := $(filter-out $(ROOT)/src/eeprom.c,$(wildcard $(ROOT)/src/*.c))
DATA_CSRCS := $(wildcard $(ROOT)/src/data/*.c)
# Data assembly. sound_data.s is excluded: the song files use `.word`, which is 4 bytes on ARM
# but 2 on SH, so songs are read in place from the mapped ROM image instead.
DATA_SSRCS := $(filter-out $(ROOT)/asm/data/sound_data.s,$(wildcard $(ROOT)/asm/data/*.s))

HAL_SRCS := $(wildcard src/*.c)

GAME_OBJS := $(patsubst $(ROOT)/src/%.c,$(BUILD)/game/%.o,$(GAME_SRCS))
DATA_OBJS := $(patsubst $(ROOT)/src/data/%.c,$(BUILD)/data/%.o,$(DATA_CSRCS)) \
             $(patsubst $(ROOT)/asm/data/%.s,$(BUILD)/data/%_s.o,$(DATA_SSRCS))
HAL_OBJS  := $(patsubst src/%.c,$(BUILD)/hal/%.o,$(HAL_SRCS)) $(BUILD)/hal/rom_blob.o $(BUILD)/hal/code_ptr_fixups.o

INC     := $(BUILD)/include
GAME_CPPFLAGS := -nostdinc -I $(ROOT)/tools/agbcc/include -iquote $(INC) -DREVISION=0 -DENGLISH -DPLATFORM_DREAMCAST
# Game code was written for agbcc (GCC 2.95, C89). Keep modern GCC from exploiting UB that
# agbcc never did.
GAME_CFLAGS := $(filter-out -Werror%,$(KOS_CFLAGS)) -std=gnu89 -fno-strict-aliasing -fwrapv \
               -fno-aggressive-loop-optimizations -fno-delete-null-pointer-checks -w

.PHONY: all clean census
all: $(TARGET)

# ---- header overlay: kleod include/ + port/dc/include on top --------------------------------
$(INC)/.stamp: $(shell find $(ROOT)/include include -type f)
	rm -rf $(INC) && mkdir -p $(INC)
	cp -r $(ROOT)/include/. $(INC)/
	cp -r include/. $(INC)/
	touch $@

# ---- game code: cpp -> kleod preproc (INCBIN, charmap strings) -> SH-4 gcc ------------------
$(BUILD)/game/%.o: $(ROOT)/src/%.c $(INC)/.stamp
	@mkdir -p $(dir $@)
	$(CC) -E $(GAME_CPPFLAGS) $< -o $(BUILD)/game/$*.i
	$(PREPROC) $(BUILD)/game/$*.i $(ROOT)/charmap.txt | sed -f patches/game.sed > $(BUILD)/game/$*.pp.c
	$(CC) $(GAME_CFLAGS) -x c -c $(BUILD)/game/$*.pp.c -o $@

$(BUILD)/data/%.o: $(ROOT)/src/data/%.c $(INC)/.stamp
	@mkdir -p $(dir $@)
	cd $(ROOT) && $(CC) -E $(GAME_CPPFLAGS) src/data/$*.c -o $(BUILD)/data/$*.i
	cd $(ROOT) && $(PREPROC) $(BUILD)/data/$*.i charmap.txt > $(BUILD)/data/$*.pp.c
	$(CC) $(GAME_CFLAGS) -x c -c $(BUILD)/data/$*.pp.c -o $@

# Data .s files are plain directives plus .incbin from baserom.gba. C symbols on sh-elf carry a
# leading underscore, so every symbol in the assembled object gets one.
$(BUILD)/data/%_s.o: $(ROOT)/asm/data/%.s
	@mkdir -p $(dir $@)
	cd $(ROOT) && $(PREPROC) asm/data/$*.s charmap.txt | $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-cpp -I include -nostdinc -undef -Wno-unicode - \
	  | $(AS) --little -I . -o $(BUILD)/data/$*_s.raw.o
	$(OBJCOPY) --prefix-symbols=_ $(BUILD)/data/$*_s.raw.o $@

# ---- hardware abstraction layer ----------------------------------------------------------
$(BUILD)/hal/%.o: src/%.c $(INC)/.stamp $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(KOS_CFLAGS) -O2 -Wall $(if $(MAX_FRAMES),-DHAL_MAX_FRAMES=$(MAX_FRAMES)) -DPLATFORM_DREAMCAST -c $< -o $@

$(BUILD)/hal/rom_blob.o: src/rom_blob.S $(ROOT)/baserom.gba
	@mkdir -p $(dir $@)
	cd $(ROOT) && $(CC) $(KOS_CFLAGS) -c $(CURDIR)/src/rom_blob.S -o $@

# Dreamcast-native copies of the jump tables whose entries kleod still has as raw incbin bytes.
$(BUILD)/hal/code_ptr_fixups.c: tools/scan_code_pointers.py $(ROOT)/kleod.elf $(ROOT)/baserom.gba
	@mkdir -p $(dir $@)
	python3 tools/scan_code_pointers.py --root $(ROOT) --emit-c $@
$(BUILD)/hal/code_ptr_fixups.o: $(BUILD)/hal/code_ptr_fixups.c
	$(CC) $(KOS_CFLAGS) -O2 -w -c $< -o $@

# ---- link ----------------------------------------------------------------------------------
# Every symbol the game objects reference but nobody defines is a fixed GBA address (RAM
# globals placed by kleod's linker.ld, ROM data read from the mapped image). Take those
# addresses from kleod.elf.
$(BUILD)/gba_symbols.ld $(BUILD)/hal/asm_stubs.c &: tools/gen_symbols.py $(GAME_OBJS) $(DATA_OBJS) $(HAL_OBJS) $(ROOT)/kleod.elf
	python3 tools/gen_symbols.py --elf $(ROOT)/kleod.elf --nm $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-nm \
	  --out $(BUILD)/gba_symbols.ld --stubs $(BUILD)/hal/asm_stubs.c --report $(BUILD)/link_report.txt \
	  $(GAME_OBJS) $(DATA_OBJS) $(HAL_OBJS)
$(BUILD)/hal/asm_stubs.o: $(BUILD)/hal/asm_stubs.c
	$(CC) $(KOS_CFLAGS) -O2 -w -iquote src -c $< -o $@

$(TARGET): $(GAME_OBJS) $(DATA_OBJS) $(HAL_OBJS) $(BUILD)/hal/asm_stubs.o $(BUILD)/gba_symbols.ld
	$(CC) $(KOS_CFLAGS) $(KOS_LDFLAGS) -o $@ $(KOS_START) $(GAME_OBJS) $(DATA_OBJS) $(HAL_OBJS) \
	  $(BUILD)/hal/asm_stubs.o -Wl,$(BUILD)/gba_symbols.ld -Wl,-Map,$(BUILD)/klonoa-dc.map $(KOS_LIBS)

census:
	python3 tools/census.py --root $(ROOT)

clean:
	rm -rf $(BUILD)
