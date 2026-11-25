CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LDLIBS ?= -lncurses -ltinfo

TARGET = sysmon
SRC = sysmon.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET)
