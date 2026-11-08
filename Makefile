# CWTNote - build instructions
# SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-CWTNote-Commercial

CC       ?= cc
PKG_CONFIG ?= pkg-config

PACKAGES := gtk4 libadwaita-1 gtksourceview-5

CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs $(PACKAGES))

BIN      := cwtnote
SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:.c=.o)
DEP      := $(OBJ:.o=.d)

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
APPDIR   ?= $(PREFIX)/share/applications
ICONDIR  ?= $(PREFIX)/share/icons/hicolor/scalable/apps
METADATADIR ?= $(PREFIX)/share/metainfo

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) $(PKG_LIBS) -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -MMD -MP -c $< -o $@

-include $(DEP)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 data/io.github.Traliran.CWTNote.desktop \
		$(DESTDIR)$(APPDIR)/io.github.Traliran.CWTNote.desktop
	install -Dm644 data/icons/hicolor/scalable/apps/io.github.Traliran.CWTNote.svg \
		$(DESTDIR)$(ICONDIR)/io.github.Traliran.CWTNote.svg
	install -Dm644 data/io.github.Traliran.CWTNote.metainfo.xml \
		$(DESTDIR)$(METADATADIR)/io.github.Traliran.CWTNote.metainfo.xml

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(APPDIR)/io.github.Traliran.CWTNote.desktop
	rm -f $(DESTDIR)$(ICONDIR)/io.github.Traliran.CWTNote.svg
	rm -f $(DESTDIR)$(METADATADIR)/io.github.Traliran.CWTNote.metainfo.xml

clean:
	rm -f $(BIN) $(OBJ) $(DEP)

.PHONY: all install uninstall clean
