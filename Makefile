ifeq ($(OS), Windows_NT)
    PLATFORM_SRC = src/platform/win32.c
    LIBS         = -lgdi32 -luser32 -lkernel32
    OUT          = segvault.exe
    MKDIR        = mkdir
    RM           = del /Q /F
    SEP          = \\
else
    PLATFORM_SRC = src/platform/xlib.c
    XFT_CFLAGS  := $(shell pkg-config --cflags xft 2>/dev/null || echo -I/usr/include/freetype2)
    XFT_LIBS    := $(shell pkg-config --libs xft xrender fontconfig 2>/dev/null || echo -lXft -lXrender -lfontconfig)
    LIBS         = -lX11 $(XFT_LIBS) -lm -pthread
    OUT          = segvault
    MKDIR        = mkdir -p
    RM           = rm -f
    SEP          = /
endif

CC       = gcc
CFLAGS   = -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -g -O2
DEPFLAGS = -MMD -MP   # file .d: un header modificato ricompila chi lo include
BASE_INCLUDES = -Iinclude -Isrc -Isrc/platform -Isrc/widgets -Isrc/catalog -Isrc/storage -Isrc/table -Isrc/index -Isrc/query -Isrc/tx -Isrc/bridge
INCLUDES = $(XFT_CFLAGS) -Iinclude -Isrc -Isrc/platform -Isrc/widgets -Isrc/catalog -Isrc/storage -Isrc/table -Isrc/index -Isrc/query -Isrc/tx -Isrc/bridge

# ── Widget / GUI source files ──
WIDGET_SRCS = \
	src/widgets/widget.c \
	src/widgets/theme.c \
	src/widgets/settings.c \
	src/widgets/result_export.c \
	src/widgets/csv_import.c \
	src/widgets/sql_text.c \
	src/widgets/button.c \
	src/widgets/label.c \
	src/widgets/textbox.c \
	src/widgets/line_edit.c \
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

.PHONY: all gui full clean test test-asan windows

all: full

gui: $(GUI_OBJS)
	$(CC) $(CFLAGS) -o $(OUT) $(GUI_OBJS) $(LIBS)
	@echo "=== GUI test build: $(OUT) ==="

full: $(FULL_OBJS)
	$(CC) $(CFLAGS) -o $(OUT) $(FULL_OBJS) $(LIBS)
	@echo "=== Full build: $(OUT) ==="

BTREE_TEST_SRCS = tests/test_btree.c src/index/btree.c src/storage/buffer_pool.c src/storage/page.c \
                  src/table/tuple.c src/catalog/schema.c

SQL_TEST_SRCS = tests/test_sql.c $(ENGINE_SRCS)

test: test_engine test_btree test_sql
	./test_btree
	./test_engine
	./test_sql

test_sql: $(SQL_TEST_SRCS)
	$(CC) $(CFLAGS) -I. $(INCLUDES) -o test_sql $(SQL_TEST_SRCS) -lm -pthread

test_btree: $(BTREE_TEST_SRCS)
	$(CC) $(CFLAGS) -I. $(INCLUDES) -o test_btree $(BTREE_TEST_SRCS) -lm

# test del motore con AddressSanitizer + UndefinedBehaviorSanitizer: fallisce su accessi fuori
# memoria, use-after-free, leak e comportamenti indefiniti (es. letture disallineate)
test-asan: test_engine.c $(ENGINE_SRCS) src/widgets/csv_import.c
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		$(INCLUDES) -o test_engine_asan test_engine.c $(ENGINE_SRCS) src/widgets/csv_import.c -lm -pthread
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(INCLUDES) -o test_btree_asan $(BTREE_TEST_SRCS) -lm
	ASAN_OPTIONS=detect_leaks=1 ./test_btree_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(INCLUDES) -o test_sql_asan $(SQL_TEST_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_engine_asan
	ASAN_OPTIONS=detect_leaks=1 ./test_sql_asan

test_engine: test_engine.c $(ENGINE_SRCS) src/widgets/csv_import.c
	$(CC) $(CFLAGS) $(INCLUDES) -o test_engine test_engine.c $(ENGINE_SRCS) src/widgets/csv_import.c -lm -pthread

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# ── Cross-compilazione Windows da Linux (MinGW-w64) ──
#   make windows  ->  segvault.exe (statico: basta copiarlo con la cartella assets/)
WIN_CC   ?= x86_64-w64-mingw32-gcc
WIN_SRCS  = src/main.c src/platform/win32.c $(WIDGET_SRCS) $(ENGINE_SRCS)
WIN_OBJS  = $(patsubst %.c,build-win/%.o,$(WIN_SRCS))

windows: $(WIN_OBJS)
	$(WIN_CC) $(CFLAGS) -o segvault.exe $(WIN_OBJS) -static -mwindows -lgdi32 -luser32 -lkernel32
	@echo "=== Windows build: segvault.exe ==="

build-win/%.o: %.c
	@mkdir -p $(dir $@)
	$(WIN_CC) $(CFLAGS) $(DEPFLAGS) $(BASE_INCLUDES) -c $< -o $@

clean:
	$(RM) $(GUI_OBJS) $(FULL_OBJS) $(GUI_OBJS:.o=.d) $(FULL_OBJS:.o=.d) $(OUT) test_engine test_engine_asan test_btree test_btree_asan test_sql test_sql_asan
	$(RM) -r obj build-win

-include $(FULL_OBJS:.o=.d) $(GUI_OBJS:.o=.d) $(WIN_OBJS:.o=.d)
