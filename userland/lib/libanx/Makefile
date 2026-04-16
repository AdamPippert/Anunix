ARCH     ?= $(shell uname -m | sed 's/arm64/aarch64/')
CC       ?= clang
CFLAGS   += -std=c11 -Wall -Wextra -Werror -ffreestanding -nostdlib \
            -I../../../kernel/include \
            -DANX_USERLAND

SRCS     = fmt.c path.c exec.c access.c stream.c
OBJS     = $(SRCS:.c=.o)
LIB      = libanx.a

all: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
