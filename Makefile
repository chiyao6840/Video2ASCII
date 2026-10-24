CC      ?= gcc
PKGS    := libavformat libavcodec libavutil libswscale libswresample sdl2

CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

TARGET  := video2ascii
SRC     := video2ascii.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
