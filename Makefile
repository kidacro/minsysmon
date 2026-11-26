CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LDLIBS ?= -lncurses -ltinfo
PREFIX ?= /usr/local
DESTDIR ?=

TARGET = sysmon
SRC = sysmon.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDLIBS)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)
