CC      = gcc
CFLAGS  = -O2 -Wall -std=c11 -D_GNU_SOURCE -DWARP_SKIP_SIG_VERIFY -Wno-unused-const-variable
LDFLAGS = -lcurl -lssl -lcrypto
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

SRC_DIR = src
SRCS    = $(SRC_DIR)/main.c \
          $(SRC_DIR)/json.c \
          $(SRC_DIR)/crypto.c \
          $(SRC_DIR)/download.c \
          $(SRC_DIR)/store.c \
          $(SRC_DIR)/index.c \
          $(SRC_DIR)/commands.c

OBJS    = $(SRCS:.c=.o)
TARGET  = warp

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Built: $(TARGET)"
	@strip $(TARGET) 2>/dev/null || true

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/warp.h
	$(CC) $(CFLAGS) -c $< -o $@

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/warp

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/warp

clean:
	rm -f $(OBJS) $(TARGET)
