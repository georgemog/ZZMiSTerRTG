# MiSTerZZ.card - ZZ9000.card built for the MiSTer Minimig Z3 RTG board.
#
#   export PATH=/opt/amiga/bin:$PATH
#   make p96-sdk      # once: fetch the Picasso96 developer headers into sdk/
#   make              # -> build/MiSTerZZ.card + tools (MiSTerZZInfo, MiSTerZZTest, SetToolType)
#
# Same compiler flags as the official rtg/build.sh.

CC       := m68k-amigaos-gcc
P96_URL  := http://wiki.icomp.de/w/images/e/e9/Develop.lha
P96_INC  ?= sdk/Picasso96Develop/Include
CFLAGS   := -m68020 -mtune=68020-60 -O2 -Iinclude -I$(P96_INC) -noixemul \
            -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer -nostartfiles
SRC      := rtg/mntgfx-gcc.c
DEPS     := $(wildcard rtg/*.h include/*.h) ../common/mister_zz9000.h

all: build/MiSTerZZ.card build/MiSTerZZInfo build/SetToolType build/MiSTerZZTest

build/MiSTerZZ.card: $(SRC) $(DEPS)
	@test -f $(P96_INC)/libraries/Picasso96.h || { echo "Picasso96 headers missing: run 'make p96-sdk' or set P96_INC"; exit 1; }
	@mkdir -p build
	$(CC) $(SRC) $(CFLAGS) -o $@ -lamiga

build/MiSTerZZInfo: tools/MiSTerZZInfo.c ../common/mister_zz9000.h
	@mkdir -p build
	$(CC) -m68020 -O2 -noixemul -Wall -o $@ $<

build/SetToolType: tools/SetToolType.c
	@mkdir -p build
	$(CC) -m68020 -O2 -noixemul -Wall -o $@ $<

build/MiSTerZZTest: tools/MiSTerZZTest.c ../common/mister_zz9000.h
	@mkdir -p build
	$(CC) -m68020 -O2 -noixemul -Wall -I$(P96_INC) -o $@ $< -lm

p96-sdk:
	mkdir -p sdk
	cd sdk && { test -f Develop.lha || wget -q $(P96_URL) -O Develop.lha; } && lha xq Develop.lha

clean:
	rm -rf build

.PHONY: all p96-sdk clean
