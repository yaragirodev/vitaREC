# VITA2REC PC-side AVI recorder
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?=

ifeq ($(OS),Windows_NT)
  CC  := x86_64-w64-mingw32-gcc
  LD  := -lws2_32 -lwinmm
else
  LD  :=
endif

TARGET := vita-recorder
TARGET := $(if $(findstring Windows_NT,$(OS)),$(TARGET).exe,$(TARGET))

$(TARGET): recorder.c
	$(CC) $(CFLAGS) -o $@ recorder.c $(LD)

windows:
	$(MAKE) CC=x86_64-w64-mingw32-gcc

clean:
	rm -f $(TARGET) *.o
