# Noteflow - build instructions
# SPDX-License-Identifier: GPL-3.0-or-later

CC       ?= cc
PKG_CONFIG ?= pkg-config

PACKAGES := gtk4 libadwaita-1 gtksourceview-5

CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs $(PACKAGES))

BIN      := noteflow
SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:.c=.o)
DEP      := $(OBJ:.o=.d)

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
APPDIR   ?= $(PREFIX)/share/applications
ICONDIR  ?= $(PREFIX)/share/icons/hicolor/scalable/apps

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) $(PKG_LIBS) -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -MMD -MP -c $< -o $@

-include $(DEP)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 data/org.noteflow.Noteflow.desktop \
		$(DESTDIR)$(APPDIR)/org.noteflow.Noteflow.desktop
	install -Dm644 data/icons/hicolor/scalable/apps/org.noteflow.Noteflow.svg \
		$(DESTDIR)$(ICONDIR)/org.noteflow.Noteflow.svg

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(APPDIR)/org.noteflow.Noteflow.desktop
	rm -f $(DESTDIR)$(ICONDIR)/org.noteflow.Noteflow.svg

clean:
	rm -f $(BIN) $(OBJ) $(DEP)

.PHONY: all install uninstall clean
