# audiostream -- the engine's sound, on a socket the browser can reach.
#
#   make            # -> linux/audiostream
#   make clean

BUILDDIR = linux

CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
STRIP  ?= strip

TARGET = $(BUILDDIR)/audiostream

all: $(TARGET)

$(TARGET): audiostream.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ audiostream.c

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

strip: $(TARGET)
	$(STRIP) $(TARGET)

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean strip
