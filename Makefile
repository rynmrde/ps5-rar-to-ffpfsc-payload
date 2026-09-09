ifneq ($(filter-out linux linux-deps test-native test-resume test-conversion-recovery test-exfat-recovery test-exfat-source-change test-archive test-url-download test-default-port benchmark-finalization archive-lib mkpfs-pfsc mkpfs-wrap-exfat mkpfs-exfat mkpfs-convert-folder compat-upstream clean,$(MAKECMDGOALS)),)
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

VERSION_TAG := v0.3.8
TITLE_ID    := FMGR88888
PYTHON      ?= python3
STRIP       ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PKG_CONFIG  ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
HOST_CC     = cc
HOST_CXX    = c++
HOST_STRIP  ?= strip
HOST_PKG_CONFIG ?= pkg-config

BIN        := rar-to-ffpfsc-ps5-payload.elf
LEGACY_BIN := web-file-mgr.elf
LINUX_BIN  := web-file-mgr-linux
COMMON_SRCS := src/main.c src/websrv.c src/filemgr.c src/file_response.c src/task.c src/upload.c src/download.c src/url_download.c src/text.c src/list.c src/space.c src/fs_util.c src/json_util.c src/path_util.c src/asset.c src/mime.c src/notify.c src/pkg_installer.c src/pkg_info.c src/mkpfs_native.c
PS5_SRCS    := $(COMMON_SRCS) src/app_installer.c
LINUX_SRCS  := $(COMMON_SRCS)
ARCHIVE_DIR := third_party/unrar-ps5
ARCHIVE_LIB := $(ARCHIVE_DIR)/libmkpfsarchive.a
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
LDADD  += -lSceIpmi -lSceAppInstUtil -lSceUserService -lSceHttp -lz
LINUX_CFLAGS := -O2 -flto -Wall -Werror -Isrc -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
LINUX_CFLAGS += `$(HOST_PKG_CONFIG) libmicrohttpd --cflags`
LINUX_LDADD := `$(HOST_PKG_CONFIG) libmicrohttpd --libs` -pthread -lz -lstdc++

.PHONY: all linux test-native test-resume test-conversion-recovery test-exfat-recovery test-exfat-source-change test-archive test-url-download test-default-port benchmark-finalization mkpfs-pfsc mkpfs-wrap-exfat mkpfs-exfat mkpfs-convert-folder compat-upstream deps linux-deps archive-lib clean

all: deps $(BIN)

linux: linux-deps $(LINUX_BIN)

archive-lib:
	$(MAKE) -C $(ARCHIVE_DIR) clean
	$(MAKE) -C $(ARCHIVE_DIR) library CC="$(if $(CC),$(CC),cc)" CXX="$(if $(CXX),$(CXX),c++)" AR="$(if $(AR),$(AR),ar)" CFLAGS="-O2 -Wno-error" CXXFLAGS="-O2 -std=c++11 -Wall -Wno-error -Wno-logical-op-parentheses -Wno-switch -Wno-dangling-else -Wno-unused-parameter -Wno-reorder"

test-native: tests/test_mkpfs_native
	./tests/test_mkpfs_native

test-resume: tests/test_mkpfs_resume
	./tests/test_mkpfs_resume

test-conversion-recovery: linux
	./tools/test_conversion_restart_recovery.sh

test-exfat-recovery: linux
	./tools/test_exfat_restart_recovery.sh

test-exfat-source-change: linux
	./tools/test_exfat_source_change_rejection.sh

test-archive: tests/test_archive_extract
	./tests/test_archive_extract.sh

test-url-download: linux
	./tools/test_url_download_http.sh

test-default-port: linux
	./tools/test_default_port.sh

benchmark-finalization:
	./tools/benchmark_finalization.sh

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
	$(MAKE) -C $(ARCHIVE_DIR) clean
	rm -rf $(BIN) $(LEGACY_BIN) $(LINUX_BIN) tests/test_mkpfs_native tests/test_mkpfs_resume tests/test_archive_extract tools/mkpfs-pfsc tools/mkpfs-wrap-exfat tools/mkpfs-exfat tools/mkpfs-convert-folder gen

gen/%.c: assets/% gen-asset-module.py | gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

$(BIN): archive-lib $(PS5_SRCS) $(GEN_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) -Wl,--whole-archive $(ARCHIVE_LIB) -Wl,--no-whole-archive $(LDADD) -lc++ -lc++abi -lunwind
	$(STRIP) $@

$(LINUX_BIN): archive-lib $(LINUX_SRCS) $(GEN_SRCS)
	$(HOST_CC) $(LINUX_CFLAGS) -o $@ $(filter %.c,$^) -Wl,--whole-archive $(ARCHIVE_LIB) -Wl,--no-whole-archive $(LINUX_LDADD)
	$(HOST_STRIP) $@

tests/test_mkpfs_native: tests/test_mkpfs_native.c src/mkpfs_native.c src/mkpfs_native.h
	$(HOST_CC) -O2 -Wall -Werror -Isrc -o $@ tests/test_mkpfs_native.c src/mkpfs_native.c -lz -pthread

tests/test_mkpfs_resume: tests/test_mkpfs_resume.c src/mkpfs_native.c src/mkpfs_native.h
	$(HOST_CC) -O2 -Wall -Werror -Isrc -o $@ tests/test_mkpfs_resume.c src/mkpfs_native.c -lz -pthread

tests/test_archive_extract: archive-lib tests/test_archive_extract.c src/archive_extract.h
	$(HOST_CC) -O2 -Wall -Werror -Isrc -o $@ tests/test_archive_extract.c -Wl,--whole-archive $(ARCHIVE_LIB) -Wl,--no-whole-archive -pthread -lstdc++

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
