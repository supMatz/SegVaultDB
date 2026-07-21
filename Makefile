ifeq ($(OS), Windows_NT)
    PLATFORM_SRC = src/platform/win32.c
    LIBS         = -lgdi32 -luser32 -lkernel32
    OUT          = segvault.exe
    MKDIR        = mkdir
    RM           = del /Q /F
    SEP          = \\
else
    PLATFORM_SRC = src/platform/xlib.c
    LIBS         = -lX11 -lm
    OUT          = segvault
    MKDIR        = mkdir -p
    RM           = rm -f
    SEP          = /
endif

CC       = gcc
CFLAGS   = -std=c11 -Wall -Wextra -Wno-unused-parameter -g -O2
INCLUDES = -Iinclude -Isrc -Isrc/platform -Isrc/widgets

# GUI test build (uses db_api_test.c stub — no DB engine required)
WIDGET_SRCS = \
	src/widgets/widget.c \
	src/widgets/button.c \
	src/widgets/label.c \
	src/widgets/textbox.c \
	src/widgets/table_view.c \
	src/widgets/tree_view.c \
	src/widgets/scrollbar.c \
	src/widgets/panel.c \
	src/widgets/splitter.c \
	src/widgets/window.c

GUI_SRCS = src/main.c $(PLATFORM_SRC) $(WIDGET_SRCS) src/bridge/db_api_test.c
GUI_OBJS = $(GUI_SRCS:.c=.o)

.PHONY: all gui clean

all: gui

gui: $(GUI_OBJS)
	$(CC) $(CFLAGS) -o $(OUT) $(GUI_OBJS) $(LIBS)
	@echo "=== Build complete: $(OUT) ==="

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	$(RM) $(GUI_OBJS) $(OUT) $(FULL_OBJS)
	$(RM) -r obj