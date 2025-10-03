CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PKG_CFLAGS := $(shell pkg-config --cflags xkbcommon 2>/dev/null)
PKG_LIBS := $(shell pkg-config --libs xkbcommon 2>/dev/null)
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := scribe-tap

.PHONY: all clean install uninstall

all: $(BIN)

check: $(BIN)
	python3 tests/test_basic.py

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(PKG_LIBS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Isrc -Iinclude -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
