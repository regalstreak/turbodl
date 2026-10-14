# TurboDL - PS4 download manager (OpenOrbis toolchain)
TITLE       := TurboDL
VERSION     := 1.00
TITLE_ID    := PKDL00001
CONTENT_ID  := IV0000-PKDL00001_00-PKGDOWNLOADER000

TOOLCHAIN   ?= $(OO_PS4_TOOLCHAIN)
ifeq ($(TOOLCHAIN),)
ifeq ($(filter host-test clean,$(MAKECMDGOALS)),)
$(error Set OO_PS4_TOOLCHAIN to your OpenOrbis PS4 toolchain (v0.5.4+) directory)
endif
endif

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  BREW  := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
  LLVM  ?= $(BREW)/opt/llvm/bin
  LLD   ?= $(BREW)/opt/lld/bin
  CDIR  := macos
  FSELF := create-fself-macos
else
  LLVM  ?= /usr/bin
  LLD   ?= /usr/bin
  CDIR  := linux
  FSELF := create-fself
endif
CC  := $(LLVM)/clang
LD  := $(LLD)/ld.lld
BIN := $(TOOLCHAIN)/bin/$(CDIR)
export OO_PS4_TOOLCHAIN := $(TOOLCHAIN)

# Runtime files taken from the OpenOrbis SDK samples at build time (not committed).
SDKSAMPLE := $(TOOLCHAIN)/samples/SDL2
SDKFONT   := $(SDKSAMPLE)/assets/fonts/VeraMono.ttf

# Elevated auth info for the daemon (system-level BGFT / AppInstUtil access).
AUTHINFO := 000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000

LIBS := -lc -lkernel -lc++ -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad -lSceSysmodule \
        -lSceFreeType -lSceNet -lSceNetCtl -lSceSsl -lSceHttp -lSceBgft -lSceAppInstUtil \
        -lSceLncUtil -lSceSystemService
DLIBS := -lc -lkernel -lSceUserService -lSceSysmodule -lSceNet -lSceNetCtl -lSceSsl -lSceHttp \
        -lSceBgft -lSceAppInstUtil -lSceLncUtil -lSceSystemService

BUILD       := build
ENGINE      := util config http dl install web core lnc
APP_OBJS    := $(patsubst %,$(BUILD)/%.o,$(ENGINE) app_main ui remote)
DAEMON_OBJS := $(patsubst %,$(BUILD)/%.o,$(ENGINE) daemon_main remote_stub)
CFLAGS  := -D__PS4__ --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wno-unused-result \
           -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -I$(BUILD) -include $(BUILD)/build_id.h -c
LDBASE  := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib

PKG := $(CONTENT_ID).pkg

all: $(PKG)

$(BUILD):
	mkdir -p $(BUILD)

# ---- generated headers ----
$(BUILD)/index_html.h: web/index.html | $(BUILD)
	cd web && xxd -i index.html | sed 's/unsigned char index_html/static const unsigned char web_index_html/; s/unsigned int index_html_len/static const unsigned int web_index_html_len/' > ../$@

# font is embedded: /app0 isn't reachable after the GoldHEN jailbreak changes our root
$(BUILD)/font_ttf.h: $(SDKFONT) | $(BUILD)
	cp $(SDKFONT) $(BUILD)/font.ttf
	cd $(BUILD) && xxd -i font.ttf | sed 's/unsigned char font_ttf/static const unsigned char font_ttf/; s/unsigned int font_ttf_len/static const unsigned int font_ttf_len/' > font_ttf.h

# fresh build id every make; app & daemon must match or the app reinstalls the daemon
$(BUILD)/build_id.h: FORCE | $(BUILD)
	@echo '#define BUILD_ID "$(shell date +%Y%m%d-%H%M%S)"' > $@.tmp; cmp -s $@.tmp $@ || mv $@.tmp $@; rm -f $@.tmp
FORCE:

$(BUILD)/web.o: $(BUILD)/index_html.h
$(BUILD)/ui.o: $(BUILD)/font_ttf.h

$(BUILD)/%.o: src/%.c src/common.h src/sce.h $(BUILD)/build_id.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

# ---- link + sign ----
$(BUILD)/app.elf: $(APP_OBJS)
	$(LD) $(APP_OBJS) -o $@ $(LDBASE) $(LIBS) $(TOOLCHAIN)/lib/crt1.o

$(BUILD)/daemon.elf: $(DAEMON_OBJS)
	$(LD) $(DAEMON_OBJS) -o $@ $(LDBASE) $(DLIBS) $(TOOLCHAIN)/lib/crt1.o

# app: normal process (a system-auth process gets too little direct memory for a framebuffer)
eboot.bin: $(BUILD)/app.elf
	$(BIN)/$(FSELF) -in=$< -out=$(BUILD)/app.oelf --eboot eboot.bin --paid 0x3800000000000011

# daemon: elevated system auth, installed by the app into /system/vsh/app/PKDL00002
assets/misc/daemon.bin: $(BUILD)/daemon.elf
	mkdir -p assets/misc
	$(BIN)/$(FSELF) -in=$< -out=$(BUILD)/daemon.oelf --eboot assets/misc/daemon.bin --paid 0x3800000000000011 --authinfo $(AUTHINFO)

# ---- SDK runtime files ----
sce_module/%.prx: $(SDKSAMPLE)/sce_module/%.prx
	mkdir -p sce_module && cp $< $@
sce_sys/about/right.sprx: $(SDKSAMPLE)/sce_sys/about/right.sprx
	mkdir -p sce_sys/about && cp $< $@

sce_sys/param.sfo: Makefile
	mkdir -p sce_sys
	$(BIN)/PkgTool.Core sfo_new $@
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(BIN)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(BIN)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

# create-gp4 only knows a fixed set of folders (sce_sys, sce_module, assets/{audio,fonts,images,misc,videos})
PKGFILES := eboot.bin assets/misc/daemon.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx \
            sce_module/libc.prx sce_module/libSceFios2.prx

pkg.gp4: $(PKGFILES)
	$(BIN)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$(PKGFILES)"

$(PKG): pkg.gp4
	$(BIN)/PkgTool.Core pkg_build $< .

# ---- host test build (macOS/Linux): engine + web panel against a fake file server ----
HOSTDIR := /tmp/turbodl_test
host-test: $(BUILD)/index_html.h $(BUILD)/build_id.h
	clang -O1 -g -fsanitize=address,undefined -DWEB_PORT=18080 -DAPP_DIR='"$(HOSTDIR)/app"' -DFPKGI_DIR='"$(HOSTDIR)/fpkgi"' \
	  -I$(BUILD) -include $(BUILD)/build_id.h -Wall -Wno-unused-result \
	  src/util.c src/config.c src/dl.c src/web.c test/mock_http.c test/host_stubs.c test/host_main.c -o $(BUILD)/host
	@echo "built $(BUILD)/host - see test/README.md"

clean:
	rm -rf $(BUILD) eboot.bin assets/misc pkg.gp4 sce_sys/param.sfo sce_sys/about sce_module $(PKG)

.PHONY: all clean host-test FORCE
