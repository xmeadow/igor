CC ?= gcc
WINCC ?= i686-w64-mingw32-gcc

CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra

SRC = src/main.c src/agent.c src/http.c src/json.c
HDR = src/agent.h src/http.h src/json.h

# Optional compile-time defaults (baked into the binary; env vars still win).
ifneq ($(strip $(LLM_API_KEY)),)
  CPPFLAGS += -DLLM_API_KEY='"$(LLM_API_KEY)"'
endif
ifneq ($(strip $(LLM_BASE_URL)),)
  CPPFLAGS += -DLLM_BASE_URL='"$(LLM_BASE_URL)"'
endif
ifneq ($(strip $(LLM_MODEL)),)
  CPPFLAGS += -DLLM_MODEL='"$(LLM_MODEL)"'
endif

OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl)
OPENSSL_LIBS   := $(shell pkg-config --libs openssl)

all: igor

igor: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SRC) -o $@ $(OPENSSL_CFLAGS) $(OPENSSL_LIBS)

win32: igor.exe

igor.exe: $(SRC) $(HDR)
	$(WINCC) $(CFLAGS) $(CPPFLAGS) -static $(SRC) -o $@ -lwinhttp

clean:
	rm -f igor igor.exe

.PHONY: all win32 clean
