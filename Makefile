CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := scribe-tap

.PHONY: all clean install uninstall

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -Isrc -Iinclude -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
