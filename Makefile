ifeq ($(OS), Windows_NT)
    PLATFORM_SRC = src/platform/win32.c
    LIBS         = -lgdi32 -luser32 -lkernel32 -lws2_32 -lshell32
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
CFLAGS   = -std=c11 -D_GNU_SOURCE -D_FORTIFY_SOURCE=2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -g -O2
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
	src/widgets/server_view.c \
	src/widgets/grid_edit.c \
	src/widgets/table_design.c \
	src/widgets/table_designer.c \
	src/widgets/schema_model.c \
	src/widgets/schema_view.c \
	src/widgets/db_tools.c \
	src/widgets/png_write.c \
	src/widgets/users_view.c \
	src/widgets/window.c

# ── server HTTP/JSON (usato anche dalla GUI, modalità "API server") ──
NET_SRCS = src/server/server.c src/server/json.c

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
	src/bridge/db_api.c \
	src/bridge/auth.c

# ── Targets ──
#   full   — app desktop (editor SQL e server API) con il motore
#   all    — full + segvault-server (default)

FULL_SRCS = src/main.c $(PLATFORM_SRC) $(WIDGET_SRCS) $(NET_SRCS) $(ENGINE_SRCS)
FULL_OBJS = $(FULL_SRCS:.c=.o)

.PHONY: all full clean test test-asan windows bench server lib

all: full server

# ── server HTTP/JSON (nessuna GUI, nessuna libreria X11): segvault-server ──
SERVER_SRCS = src/server/main.c $(NET_SRCS) src/widgets/sql_text.c $(ENGINE_SRCS)
SERVER_OBJS = $(patsubst %.c,build-server/%.o,$(SERVER_SRCS))

server: segvault-server

segvault-server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) -lm -pthread
	@echo "=== Server build: segvault-server ==="

build-server/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(BASE_INCLUDES) -c $< -o $@

# ── libreria statica del motore, per incorporarlo in un'app C/C++ (come SQLite): libsegvault.a ──
LIB_OBJS = $(patsubst %.c,build-server/%.o,$(ENGINE_SRCS))

lib: libsegvault.a

libsegvault.a: $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)
	@echo "=== Library: libsegvault.a (header: src/bridge/db_api.h) ==="

full: $(FULL_OBJS)
	$(CC) $(CFLAGS) -o $(OUT) $(FULL_OBJS) $(LIBS)
	@echo "=== Full build: $(OUT) ==="

BTREE_TEST_SRCS = tests/test_btree.c src/index/btree.c src/storage/buffer_pool.c src/storage/page.c \
                  src/table/tuple.c src/catalog/schema.c

SQL_TEST_SRCS = tests/test_sql.c $(ENGINE_SRCS)

EXAMPLES_TEST_SRCS = tests/test_examples.c src/widgets/sql_text.c $(ENGINE_SRCS)

test: test_engine test_btree test_sql test_examples test_server test_grid_edit test_table_design test_schema_model test_db_tools test_png test_auth
	./test_btree
	./test_engine
	./test_sql
	./test_examples
	./test_server
	./test_grid_edit
	./test_table_design
	./test_schema_model
	./test_db_tools
	./test_png
	./test_auth

TEST_DESIGN_SRCS = tests/test_table_design.c src/widgets/table_design.c $(ENGINE_SRCS)

# designer di tabelle: CREATE / ALTER generati ed eseguiti sul motore
test_table_design: $(TEST_DESIGN_SRCS) src/widgets/table_design.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_table_design $(TEST_DESIGN_SRCS) -lm -pthread

TEST_SCHEMA_SRCS = tests/test_schema_model.c src/widgets/schema_model.c $(ENGINE_SRCS)

# diagramma dello schema: relazioni, posizioni, layout e SQL delle azioni
test_schema_model: $(TEST_SCHEMA_SRCS) src/widgets/schema_model.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_schema_model $(TEST_SCHEMA_SRCS) -lm -pthread

TEST_TOOLS_SRCS = tests/test_db_tools.c src/widgets/db_tools.c $(ENGINE_SRCS)

# ricerca in tutto il database e panoramica delle tabelle
test_db_tools: $(TEST_TOOLS_SRCS) src/widgets/db_tools.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_db_tools $(TEST_TOOLS_SRCS) -lm -pthread

# encoder PNG (esportazione del diagramma)
test_png: tests/test_png.c src/widgets/png_write.c src/widgets/png_write.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_png tests/test_png.c src/widgets/png_write.c

TEST_AUTH_SRCS = tests/test_auth.c $(ENGINE_SRCS)

# utenti e permessi
test_auth: $(TEST_AUTH_SRCS) src/bridge/auth.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_auth $(TEST_AUTH_SRCS) -lm -pthread

TEST_GRID_SRCS = tests/test_grid_edit.c src/widgets/grid_edit.c $(ENGINE_SRCS)

# modifica dei dati nella griglia: SQL generato ed eseguito sul motore
test_grid_edit: $(TEST_GRID_SRCS) src/widgets/grid_edit.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_grid_edit $(TEST_GRID_SRCS) -lm -pthread

TEST_SERVER_SRCS = tests/test_server.c src/server/server.c src/server/json.c src/widgets/sql_text.c $(ENGINE_SRCS)

# il server HTTP: richieste reali su un socket locale
test_server: $(TEST_SERVER_SRCS) src/server/server.h src/server/json.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_server $(TEST_SERVER_SRCS) -lm -pthread

# gli script di examples/ eseguiti come nell'editor
test_examples: $(EXAMPLES_TEST_SRCS) $(wildcard examples/*.sql)
	$(CC) $(CFLAGS) -I. $(INCLUDES) -o test_examples $(EXAMPLES_TEST_SRCS) -lm -pthread

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
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(INCLUDES) -o test_examples_asan $(EXAMPLES_TEST_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_engine_asan
	ASAN_OPTIONS=detect_leaks=1 ./test_sql_asan
	ASAN_OPTIONS=detect_leaks=1 ./test_examples_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_server_asan $(TEST_SERVER_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_server_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_grid_edit_asan $(TEST_GRID_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_grid_edit_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_table_design_asan $(TEST_DESIGN_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_table_design_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_schema_model_asan $(TEST_SCHEMA_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_schema_model_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_db_tools_asan $(TEST_TOOLS_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_db_tools_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_png_asan tests/test_png.c src/widgets/png_write.c
	ASAN_OPTIONS=detect_leaks=1 ./test_png_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_auth_asan $(TEST_AUTH_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_auth_asan

test_engine: test_engine.c $(ENGINE_SRCS) src/widgets/csv_import.c
	$(CC) $(CFLAGS) $(INCLUDES) -o test_engine test_engine.c $(ENGINE_SRCS) src/widgets/csv_import.c -lm -pthread

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# ── Cross-compilazione Windows da Linux (MinGW-w64) ──
#   make windows  ->  segvault.exe (statico: basta copiarlo con la cartella assets/)
WIN_CC   ?= x86_64-w64-mingw32-gcc
WIN_SRCS  = src/main.c src/platform/win32.c $(WIDGET_SRCS) $(NET_SRCS) $(ENGINE_SRCS)
WIN_OBJS  = $(patsubst %.c,build-win/%.o,$(WIN_SRCS))

WIN_SERVER_OBJS = $(patsubst %.c,build-win/%.o,$(SERVER_SRCS))

windows: $(WIN_OBJS) $(WIN_SERVER_OBJS)
	$(WIN_CC) $(CFLAGS) -o segvault.exe $(WIN_OBJS) -static -mwindows -lgdi32 -luser32 -lkernel32 -lws2_32 -lshell32
	$(WIN_CC) $(CFLAGS) -o segvault-server.exe $(WIN_SERVER_OBJS) -static -lws2_32
	@echo "=== Windows build: segvault.exe, segvault-server.exe ==="

build-win/%.o: %.c
	@mkdir -p $(dir $@)
	$(WIN_CC) $(CFLAGS) $(DEPFLAGS) $(BASE_INCLUDES) -c $< -o $@

clean:
	$(RM) $(FULL_OBJS) $(FULL_OBJS:.o=.d) $(OUT) test_engine test_engine_asan test_btree test_btree_asan test_sql test_sql_asan bench_sql test_examples test_examples_asan
	$(RM) segvault-server segvault-server.exe libsegvault.a test_server test_server_asan test_grid_edit test_grid_edit_asan test_table_design test_table_design_asan test_schema_model test_schema_model_asan test_db_tools test_db_tools_asan test_png test_png_asan test_auth test_auth_asan
	$(RM) -r obj build-win build-server

-include $(FULL_OBJS:.o=.d) $(WIN_OBJS:.o=.d) $(SERVER_OBJS:.o=.d) $(WIN_SERVER_OBJS:.o=.d)

# ── benchmark del motore (schema universitario, righe configurabili: ./bench_sql 100000) ──
bench_sql: bench/bench.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -I. $(INCLUDES) -o bench_sql bench/bench.c $(ENGINE_SRCS) -lm -pthread

bench: bench_sql
	./bench_sql 100000
