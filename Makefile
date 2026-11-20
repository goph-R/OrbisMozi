# Orbis Mozi - a video player for the PS4 (FFmpeg + OpenOrbis toolchain)
#
#   make          build IV0000-MOZI00001_00-ORBISMOZI0000000.pkg
#   make host     build the PC test binary:  build/player_host <file> [seconds]
#   make clean
#
# See README.md for how to set up the toolchain and FFmpeg.

TITLE       := Orbis Mozi
VERSION     := 1.00
TITLE_ID    := MOZI00001
CONTENT_ID  := IV0000-$(TITLE_ID)_00-ORBISMOZI0000000

# Everything the build needs lives under one folder (override: make PS4TC=/path).
PS4TC       ?= $(abspath ../PS4-Toolchain)
OO_PS4_TOOLCHAIN ?= $(PS4TC)/OpenOrbis/PS4Toolchain
TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
export OO_PS4_TOOLCHAIN
FFMPEG      ?= $(PS4TC)/ffmpeg-ps4
FFMPEG_HOST ?= $(PS4TC)/ffmpeg-host
SMB2        ?= $(PS4TC)/libsmb2-ps4
SMB2_HOST   ?= $(PS4TC)/libsmb2-host
LD          := $(or $(shell command -v ld.lld),$(PS4TC)/lld/usr/lib/llvm-18/bin/ld.lld)
CC          := clang
TOOLS       := $(TOOLCHAIN)/bin/linux

# PkgTool.Core is an old .NET build that needs libssl 1.1; SSL11 may point at
# an unpacked copy (see README). Harmless if the folder does not exist.
SSL11       ?= $(PS4TC)/libssl11/usr/lib/x86_64-linux-gnu
PKGTOOL     := DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 LD_LIBRARY_PATH=$(SSL11) $(TOOLS)/PkgTool.Core

FFLIBS      := -lavformat -lavcodec -lswscale -lswresample -lavutil
LIBS        := $(FFLIBS) -lsmb2 -lm -lc -lkernel -lSceVideoOut -lSceAudioOut -lSceUserService -lScePad -lSceNet
CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -march=btver2 \
               -Wall -Wextra -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -I$(FFMPEG)/include -I$(SMB2)/include
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr \
               --wrap=pthread_create --wrap=fcntl -L$(FFMPEG)/lib -L$(SMB2)/lib -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

BUILD       := build
SRCS        := $(wildcard src/*.c)
OBJS        := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

# System files every package needs. They are not kept in this repository;
# they are copied from the toolchain's samples on first build.
SAMPLE      := $(TOOLCHAIN)/samples/hello_world
SYSFILES    := sce_sys/about/right.sprx sce_module/libc.prx sce_module/libSceFios2.prx

# Optional: videos placed in assets/videos/ are packed into the .pkg and
# played if nothing is found in /data/video (handy for a self-contained test).
ASSETS      := $(wildcard assets/videos/*.*)

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(PKGTOOL) pkg_build $< .

pkg.gp4: eboot.bin sce_sys/param.sfo sce_sys/icon0.png $(SYSFILES) $(ASSETS)
	$(TOOLS)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

$(SYSFILES): %: $(SAMPLE)/%
	mkdir -p $(dir $@)
	cp $< $@

sce_sys/param.sfo: Makefile
	$(PKGTOOL) sfo_new $@
	$(PKGTOOL) sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(PKGTOOL) sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(PKGTOOL) sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(PKGTOOL) sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(PKGTOOL) sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(PKGTOOL) sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(PKGTOOL) sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(OBJS)
	$(LD) $(OBJS) -o $(BUILD)/player.elf $(LDFLAGS)
	$(TOOLS)/create-fself -in=$(BUILD)/player.elf -out=$(BUILD)/player.oelf --eboot "eboot.bin" --paid 0x3800000000000011

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

# PC test build: same player core, fake display and audio device.
HOST_SRCS := src/main.c src/player.c src/osd.c src/browser.c src/smb.c host/plat_host.c
host: $(BUILD)/player_host
$(BUILD)/player_host: $(HOST_SRCS) $(wildcard src/*.h) | $(BUILD)
	clang -O2 -g -Wall -Wextra -I$(FFMPEG_HOST)/include -I$(SMB2_HOST)/include -o $@ $(HOST_SRCS) \
	      -L$(FFMPEG_HOST)/lib -L$(SMB2_HOST)/lib $(FFLIBS) -lsmb2 -lm -lpthread

clean:
	rm -rf $(BUILD) *.pkg pkg.gp4 eboot.bin sce_sys/param.sfo

.PHONY: all host clean
