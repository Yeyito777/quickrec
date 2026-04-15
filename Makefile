CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS = -lX11 -lXext
TARGET  = quickrec
SRC     = main.c
PREFIX  = $(HOME)/.local/bin

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	mkdir -p $(PREFIX)
	cp -f $(TARGET) $(PREFIX)/$(TARGET)
	chmod 755 $(PREFIX)/$(TARGET)

clean:
	rm -f $(TARGET)
