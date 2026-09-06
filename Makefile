.POSIX:

CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PKGS = wayland-client freetype2 fontconfig
BAR_CFLAGS != $(PKG_CONFIG) --cflags $(PKGS)
BAR_LIBS != $(PKG_CONFIG) --libs $(PKGS)

WAYLAND_PROTOCOLS != $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null || echo "/usr/share/wayland-protocols"

ALL_CFLAGS = $(CFLAGS) $(BAR_CFLAGS) -I.
ALL_LIBS = $(BAR_LIBS)

all: mintbar

release: CFLAGS += -O2 -DNDEBUG
release: LDFLAGS += -s
release: all

wlr-layer-shell-unstable-v1-client-protocol.h: wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

wlr-layer-shell-unstable-v1-protocol.c: wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

config.h:
	cp config.def.h $@

wlr-layer-shell-unstable-v1-protocol.o: wlr-layer-shell-unstable-v1-protocol.c wlr-layer-shell-unstable-v1-client-protocol.h
	$(CC) -c $< $(CFLAGS) -o $@

xdg-shell-protocol.o: xdg-shell-protocol.c
	$(CC) -c $< $(CFLAGS) -o $@

mintbar.o: mintbar.c config.h wlr-layer-shell-unstable-v1-client-protocol.h
	$(CC) -c $< $(ALL_CFLAGS) -o $@

mintbar: mintbar.o wlr-layer-shell-unstable-v1-protocol.o xdg-shell-protocol.o
	$(CC) $^ $(LDFLAGS) $(ALL_LIBS) -o $@

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 mintbar $(DESTDIR)$(BINDIR)/mintbar

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/mintbar

clean:
	rm -f mintbar mintbar.o wlr-layer-shell-unstable-v1-protocol.o xdg-shell-protocol.o \
		wlr-layer-shell-unstable-v1-client-protocol.h \
		wlr-layer-shell-unstable-v1-protocol.c \
		xdg-shell-protocol.c

.PHONY: all release install uninstall clean
