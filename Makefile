src = $(wildcard src/*.c)
obj = $(src:.c=.o)

PREFIX ?= /usr/local
CONFIG_PREFIX ?= /etc

CFLAGS = -I./include -DCONFIG_PREFIX=\"$(CONFIG_PREFIX)"
LDFLAGS = -O2

all: wake-on-arp-alt

debug: LDFLAGS = -static-libasan
debug: CFLAGS += -g -DDEBUG -Wall -Wextra -fsanitize=address,undefined,pointer-compare,pointer-subtract -fno-omit-frame-pointer
debug: wake-on-arp-alt

wake-on-arp-alt: $(obj)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

install:
	install -Dm755 wake-on-arp-alt $(PREFIX)/bin/wake-on-arp-alt
	test -f $(CONFIG_PREFIX)/wake-on-arp-alt.conf || install -Dm600 example.conf $(CONFIG_PREFIX)/wake-on-arp-alt.conf

.PHONY: clean
clean:
	rm -f $(obj) wake-on-arp-alt
