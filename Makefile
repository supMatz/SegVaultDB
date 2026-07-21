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
CFLAGS   = -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -g -O2
INCLUDES = -Iinclude -Isrc -Isrc/platform -Isrc/widgets -Isrc/catalog -Isrc/storage -Isrc/table -Isrc/index -Isrc/query -Isrc/tx -Isrc/bridge

# ── Widget / GUI source files ──
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

# ── DB engine source files ──
ENGINE_SRCS = \
	src/catalog/schema.c \
	src/storage/page.c \
	src/storage/buffer_pool.c \
	src/table/tuple.c \
	src/table/heap.c \
	src/index/btree.c \
	src/query/lexer.c \
	src/query/parser.c \
	src/query/executor.c \
	src/tx/wal.c \
	src/tx/transaction.c \
	src/bridge/db_api.c

# ── Targets ──
#   gui    — GUI test (no real DB engine, uses db_api_test.c stub)
#   full   — Full SegVault DBMS with real engine
#   all    — full (default)

GUI_SRCS  = src/main.c $(PLATFORM_SRC) $(WIDGET_SRCS) src/bridge/db_api_test.c
GUI_OBJS  = $(GUI_SRCS:.c=.o)

FULL_SRCS = src/main.c $(PLATFORM_SRC) $(WIDGET_SRCS) $(ENGINE_SRCS)
FULL_OBJS = $(FULL_SRCS:.c=.o)

.PHONY: all gui full clean

all: full

gui: $(GUI_OBJS)
	$(CC) $(CFLAGS) -o $(OUT) $(GUI_OBJS) $(LIBS)
	@echo "=== GUI test build: $(OUT) ==="

full: $(FULL_OBJS)
	$(CC) $(CFLAGS) -o $(OUT) $(FULL_OBJS) $(LIBS)
	@echo "=== Full build: $(OUT) ==="

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	$(RM) $(GUI_OBJS) $(FULL_OBJS) $(OUT)
	$(RM) -r obj