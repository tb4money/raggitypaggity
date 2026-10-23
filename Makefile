# CXPS5 build.
#   make NO_CURL=1  same, without link-downloads (if your SDK has no curl port)
#   make            PS5 payload -> cxps5.elf  (needs ps5-payload-sdk with the curl/openssl ports; set PS5_PAYLOAD_SDK)
#   make host       Linux/macOS build for testing the loader on a computer -> build/cxps5-host
#   make test       host build + end-to-end API test (needs python3)

PS5_SDK ?= $(PS5_PAYLOAD_SDK)
ELF  := cxps5.elf
HOST := build/cxps5-host
SRC  := src/cxps5.c
UI   := ui/index.html
CACERT := $(abspath third_party/cacert.pem)
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation

build/ui_html.h: $(UI) tools/embed.py
	@mkdir -p build
	python3 tools/embed.py $(UI) $@ UI_HTML

all: $(ELF)

ifeq ($(NO_CURL),1)
PS5_DEFS :=
PS5_LIBS := -lz
else
PS5_DEFS := -DHAVE_CURL -DCACERT_PATH='"$(CACERT)"'
PS5_LIBS := -lcurl -lssl -lcrypto -lz -lzstd -lpsl
endif

$(ELF): $(SRC) build/ui_html.h third_party/cacert.pem
	@test -n "$(PS5_SDK)" || { echo "set PS5_PAYLOAD_SDK to your ps5-payload-sdk install"; exit 1; }
	$(PS5_SDK)/bin/prospero-clang -O2 -std=gnu11 $(WARN) -Ibuild $(PS5_DEFS) \
	  -isystem $(PS5_SDK)/target/user/homebrew/include -o $@ $(SRC) $(PS5_LIBS)

host: $(HOST)
$(HOST): $(SRC) build/ui_html.h
	$(CC) -O1 -g -std=gnu11 $(WARN) -Ibuild -o $@ $(SRC) -lz -lpthread

test: $(HOST)
	python3 scripts/test_host.py $(HOST)

clean:
	rm -rf build $(ELF)

.PHONY: all host test clean
