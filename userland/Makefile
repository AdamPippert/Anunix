ARCH     ?= $(shell uname -m | sed 's/arm64/aarch64/')
CC       ?= clang
CFLAGS   += -std=c11 -Wall -Wextra -Werror -ffreestanding -nostdlib \
            -I../kernel/include \
            -DANX_USERLAND

LIBANX   = lib/libanx/libanx.a

TIER1_BINS = bin/echo bin/cat bin/ls bin/cp bin/mv bin/rm \
             bin/mkdir bin/stat bin/env bin/ps bin/kill bin/find \
             bin/grep bin/df bin/dmesg bin/sh

AGENT_BINS = hwd celf objstore/objls objstore/objcat objstore/objprov \
             engctl capctl credctl rstat intent mem decay

all: $(LIBANX) $(TIER1_BINS) $(AGENT_BINS)

$(LIBANX):
	$(MAKE) -C lib/libanx ARCH=$(ARCH) CC=$(CC) CFLAGS="$(CFLAGS)"

$(TIER1_BINS): $(LIBANX)
	$(MAKE) -C $(dir $@) ARCH=$(ARCH) CC=$(CC) CFLAGS="$(CFLAGS)"

$(AGENT_BINS): $(LIBANX)
	$(MAKE) -C $(dir $@) ARCH=$(ARCH) CC=$(CC) CFLAGS="$(CFLAGS)"

clean:
	$(MAKE) -C lib/libanx clean
	for d in bin/echo bin/cat bin/ls bin/cp bin/mv bin/rm \
	          bin/mkdir bin/stat bin/env bin/ps bin/kill bin/find \
	          bin/grep bin/df bin/dmesg bin/sh \
	          hwd celf objstore engctl capctl credctl rstat intent mem decay; \
	do $(MAKE) -C $$d clean 2>/dev/null || true; done

.PHONY: all clean $(TIER1_BINS) $(AGENT_BINS)
