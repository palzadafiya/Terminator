CC ?= gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Wpedantic -O2 -g
CFLAGS += $(shell pkg-config --cflags x11 2>/dev/null)
LDLIBS += $(shell pkg-config --libs x11 2>/dev/null) -lpthread

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDLIBS += -lrt
endif

SHELL_OBJS = src/shell/vars.o \
             src/shell/jobs.o \
             src/shell/sink.o \
             src/shell/lexer.o \
             src/shell/expand.o \
             src/shell/cmd.o \
             src/shell/builtins.o \
             src/shell/exec.o \
             src/shell/multiwatch.o \
             src/shell/shell.o

SRCS = src/main.c \
       src/proto.c \
       src/shared.c \
       src/history.c \
       src/lineedit.c \
       src/keymap.c \
       src/tabbuf.c \
       src/tabrt.c \
       src/complete.c \
       src/gui.c \
       src/shell/vars.c \
       src/shell/jobs.c \
       src/shell/sink.c \
       src/shell/lexer.c \
       src/shell/expand.c \
       src/shell/cmd.c \
       src/shell/builtins.c \
       src/shell/exec.c \
       src/shell/multiwatch.c \
       src/shell/shell.c

OBJS = $(SRCS:.c=.o)
TARGET = terminator

HARNESS_OBJS = tests/harness.o \
               src/proto.o \
               src/shared.o \
               src/history.o \
               src/lineedit.o \
               src/keymap.o \
               src/tabbuf.o \
               src/tabrt.o \
               src/complete.o \
               $(SHELL_OBJS)

HARNESS_TARGET = test_harness

.PHONY: all debug test clean

all: $(TARGET) $(HARNESS_TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(HARNESS_TARGET): $(HARNESS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

debug: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
debug: clean all

test: all
	./$(HARNESS_TARGET)

clean:
	rm -rf $(OBJS) tests/harness.o $(TARGET) $(HARNESS_TARGET) test_check_x *.dSYM tests/*.dSYM
