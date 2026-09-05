ifneq ($(filter-out linux linux-deps test-native mkpfs-pfsc mkpfs-wrap-exfat mkpfs-exfat mkpfs-convert-folder compat-upstream clean,$(MAKECMDGOALS)),)
  ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
  else
    $(error PS5_PAYLOAD_SDK is undefined)
  endif
endif
ifeq ($(MAKECMDGOALS),)
  ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
  else
    $(error PS5_PAYLOAD_SDK is undefined)
  endif
endif

VERSION_TAG := v1.7
TITLE_ID    := FMGR88888
PYTHON      ?= python3
STRIP       ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PKG_CONFIG  ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
HOST_CC     = cc
HOST_STRIP  ?= strip
HOST_PKG_CONFIG ?= pkg-config

BIN        := web-file-mgr.elf
LINUX_BIN  := web-file-mgr-linux
COMMON_SRCS := src/main.c src/websrv.c src/filemgr.c src/file_response.c src/task.c src/upload.c src/download.c src/text.c src/list.c src/space.c src/fs_util.c src/json_util.c src/path_util.c src/asset.c src/mime.c src/notify.c src/pkg_installer.c src/pkg_info.c src/mkpfs_native.c
PS5_SRCS    := $(COMMON_SRCS) src/app_installer.c
LINUX_SRCS  := $(COMMON_SRCS)
BASE_ASSETS := $(filter-out %.dds,$(wildcard assets/*))
ifneq ($(filter linux,$(MAKECMDGOALS)),)
ASSETS      := $(BASE_ASSETS)
else
ASSETS      := $(filter-out assets/icon0.png,$(BASE_ASSETS))
endif
GEN_SRCS    := $(patsubst assets/%,gen/%, $(ASSETS:=.c))

CFLAGS := -Oz -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Werror -ffunction-sections -fdata-sections -Isrc -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
CFLAGS += `$(PKG_CONFIG) libmicrohttpd --cflags`
LDFLAGS := -Wl,--gc-sections
LDADD  := `$(PKG_CONFIG) libmicrohttpd --libs`
LDADD  += -lSceIpmi -lSceAppInstUtil -lSceUserService -lz
LINUX_CFLAGS := -O2 -flto -Wall -Werror -Isrc -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
LINUX_CFLAGS += `$(HOST_PKG_CONFIG) libmicrohttpd --cflags`
LINUX_LDADD := `$(HOST_PKG_CONFIG) libmicrohttpd --libs` -pthread -lz

.PHONY: all linux test-native mkpfs-pfsc mkpfs-wrap-exfat mkpfs-exfat mkpfs-convert-folder compat-upstream deps linux-deps clean

all: deps $(BIN)

linux: linux-deps $(LINUX_BIN)

test-native: tests/test_mkpfs_native
	./tests/test_mkpfs_native

mkpfs-pfsc: tools/mkpfs-pfsc

mkpfs-wrap-exfat: tools/mkpfs-wrap-exfat

mkpfs-exfat: tools/mkpfs-exfat

mkpfs-convert-folder: tools/mkpfs-convert-folder

deps:
	@$(PKG_CONFIG) --exists libmicrohttpd || ./install-libmicrohttpd.sh

linux-deps:
	@$(HOST_PKG_CONFIG) --exists libmicrohttpd || \
	  (echo "libmicrohttpd development package is required for make linux" >&2; exit 1)

gen:
	mkdir gen

clean:
	rm -rf $(BIN) $(LINUX_BIN) tests/test_mkpfs_native tools/mkpfs-pfsc tools/mkpfs-wrap-exfat tools/mkpfs-exfat tools/mkpfs-convert-folder gen

gen/%.c: assets/% gen-asset-module.py | gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

$(BIN): $(PS5_SRCS) $(GEN_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDADD)
	$(STRIP) $@

$(LINUX_BIN): $(LINUX_SRCS) $(GEN_SRCS)
	$(HOST_CC) $(LINUX_CFLAGS) -o $@ $^ $(LINUX_LDADD)
	$(HOST_STRIP) $@

tests/test_mkpfs_native: tests/test_mkpfs_native.c src/mkpfs_native.c src/mkpfs_native.h
	$(HOST_CC) -O2 -Wall -Werror -Isrc -o $@ tests/test_mkpfs_native.c src/mkpfs_native.c -lz -pthread

tools/mkpfs-pfsc: tools/mkpfs-pfsc.c src/mkpfs_native.c src/mkpfs_native.h
	$(HOST_CC) -O2 -Wall -Werror -Isrc -o $@ tools/mkpfs-pfsc.c src/mkpfs_native.c -lz -pthread

tools/mkpfs-wrap-exfat: tools/mkpfs-wrap-exfat.c src/mkpfs_native.c src/mkpfs_native.h
	$(HOST_CC) -O2 -Wall -Werror -Isrc -o $@ tools/mkpfs-wrap-exfat.c src/mkpfs_native.c -lz -pthread

tools/mkpfs-exfat: tools/mkpfs-exfat.c src/mkpfs_native.c src/mkpfs_native.h
	$(HOST_CC) -O2 -Wall -Werror -Isrc -o $@ tools/mkpfs-exfat.c src/mkpfs_native.c -lz -pthread

tools/mkpfs-convert-folder: tools/mkpfs-convert-folder.c src/mkpfs_native.c src/mkpfs_native.h
	$(HOST_CC) -O2 -Wall -Werror -Isrc -o $@ tools/mkpfs-convert-folder.c src/mkpfs_native.c -lz -pthread

compat-upstream: mkpfs-convert-folder
	./tests/test_folder_compat.sh
