ifeq ($(OS), Windows_NT)
    PLATFORM_SRC = src/platform/win32.c src/platform/fonts_embed.c
    LIBS         = -lgdi32 -luser32 -lkernel32 -lws2_32 -lshell32 -liphlpapi
    OUT          = segvault.exe
    MKDIR        = mkdir
    RM           = del /Q /F
    SEP          = \\
else
    PLATFORM_SRC = src/platform/xlib.c src/platform/fonts_embed.c
    XFT_CFLAGS  := $(shell pkg-config --cflags xft 2>/dev/null || echo -I/usr/include/freetype2)
    XFT_LIBS    := $(shell pkg-config --libs xft xrender fontconfig 2>/dev/null || echo -lXft -lXrender -lfontconfig)
    LIBS         = -lX11 $(XFT_LIBS) -lm -pthread -ldl
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
	src/widgets/export_bin.c \
	src/widgets/chart_view.c \
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
	src/widgets/users_view.c \
	src/widgets/mysql_import.c \
	src/widgets/window.c

# ── server HTTP/JSON (usato anche dalla GUI, modalità "API server") ──
NET_SRCS = src/server/server.c src/server/json.c src/server/mysql_wire.c src/server/fastcgi.c src/server/proxy.c src/server/mail.c src/server/rewrite.c src/server/tls.c src/server/h2.c src/widgets/png_write.c

# ── DB engine source files ──
ENGINE_SRCS = \
	src/catalog/schema.c \
	src/storage/page.c \
	src/storage/buffer_pool.c \
	src/table/tuple.c \
	src/table/heap.c \
	src/table/overflow.c \
	src/index/btree.c \
	src/query/lexer.c \
	src/query/parser.c \
	src/query/executor.c \
	src/query/sqljson.c \
	src/query/regex.c \
	src/tx/wal.c \
	src/tx/transaction.c \
	src/bridge/db_api.c \
	src/bridge/locks.c \
	src/bridge/auth.c

# ── Targets ──
#   full   — app desktop (editor SQL e server API) con il motore
#   all    — full + segvault-server (default)

FULL_SRCS = src/main.c $(PLATFORM_SRC) $(WIDGET_SRCS) $(NET_SRCS) $(ENGINE_SRCS)
FULL_OBJS = $(FULL_SRCS:.c=.o)

.PHONY: all full clean test test-asan test-tsan windows bench server lib dist

all: full server

# ── server HTTP/JSON (nessuna GUI, nessuna libreria X11): segvault-server ──
SERVER_SRCS = src/server/main.c src/server/service.c $(NET_SRCS) src/widgets/sql_text.c $(ENGINE_SRCS)
SERVER_OBJS = $(patsubst %.c,build-server/%.o,$(SERVER_SRCS))

server: segvault-server

segvault-server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) -lm -pthread -ldl
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

test: test_engine test_btree test_sql test_mysql test_examples test_server test_web test_mail test_tls test_php test_grid_edit test_table_design test_schema_model test_db_tools test_png test_auth test_sessions test_parallel test_mysql_import test_overflow test_backup test_export
	./test_btree
	./test_engine
	./test_sql
	./test_mysql
	./test_examples
	./test_server
	./test_web
	./test_mail
	./test_tls
	./test_php
	./test_grid_edit
	./test_table_design
	./test_schema_model
	./test_db_tools
	./test_png
	./test_auth
	./test_sessions
	./test_parallel
	./test_mysql_import
	./test_overflow
	./test_backup
	./test_export

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

TEST_SESS_SRCS = tests/test_sessions.c $(ENGINE_SRCS)

# sessioni: stato separato e proprietario unico delle transazioni
test_sessions: $(TEST_SESS_SRCS)
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_sessions $(TEST_SESS_SRCS) -lm -pthread

TEST_PAR_SRCS = tests/test_parallel.c $(ENGINE_SRCS)

# letture in parallelo (anche con ThreadSanitizer: make test-tsan)
test_parallel: $(TEST_PAR_SRCS)
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_parallel $(TEST_PAR_SRCS) -lm -pthread

test-tsan: $(TEST_PAR_SRCS)
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=thread -I. $(BASE_INCLUDES) -o test_parallel_tsan $(TEST_PAR_SRCS) -lm -pthread
	./test_parallel_tsan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=thread -I. $(BASE_INCLUDES) -o test_php_tsan $(TEST_PHP_SRCS) -lm -pthread -ldl
	./test_php_tsan

TEST_MY_SRCS = tests/test_mysql_import.c src/widgets/mysql_import.c src/widgets/sql_text.c $(ENGINE_SRCS)

# import dei dump di MySQL
test_mysql_import: $(TEST_MY_SRCS) src/widgets/mysql_import.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_mysql_import $(TEST_MY_SRCS) -lm -pthread

TEST_OVF_SRCS = tests/test_overflow.c $(ENGINE_SRCS)
TEST_BK_SRCS = tests/test_backup.c $(ENGINE_SRCS)
TEST_EXP_SRCS = tests/test_export.c src/widgets/export_bin.c $(ENGINE_SRCS)

test_export: $(TEST_EXP_SRCS)
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_export $(TEST_EXP_SRCS) -lm -pthread


test_backup: $(TEST_BK_SRCS)
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_backup $(TEST_BK_SRCS) -lm -pthread


# righe grandi (pagine di overflow), anche dopo ROLLBACK, riavvio e crash
test_overflow: $(TEST_OVF_SRCS)
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_overflow $(TEST_OVF_SRCS) -lm -pthread

TEST_GRID_SRCS = tests/test_grid_edit.c src/widgets/grid_edit.c $(ENGINE_SRCS)

# modifica dei dati nella griglia: SQL generato ed eseguito sul motore
test_grid_edit: $(TEST_GRID_SRCS) src/widgets/grid_edit.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_grid_edit $(TEST_GRID_SRCS) -lm -pthread

TEST_SERVER_SRCS = tests/test_server.c src/server/server.c src/server/json.c src/server/mysql_wire.c src/server/fastcgi.c src/server/proxy.c src/server/mail.c src/server/rewrite.c src/server/tls.c src/server/h2.c src/widgets/png_write.c src/widgets/sql_text.c $(ENGINE_SRCS)

# il server HTTP: richieste reali su un socket locale
test_server: $(TEST_SERVER_SRCS) src/server/server.h src/server/json.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_server $(TEST_SERVER_SRCS) -lm -pthread -ldl

TEST_WEB_SRCS = tests/test_web.c src/server/server.c src/server/json.c src/server/mysql_wire.c src/server/fastcgi.c src/server/proxy.c src/server/mail.c src/server/rewrite.c src/server/tls.c src/server/h2.c src/widgets/png_write.c src/widgets/sql_text.c $(ENGINE_SRCS)

# siti per nome, cruscotto, elenco dei file, reverse proxy (anche WebSocket)
test_web: $(TEST_WEB_SRCS) src/server/server.h src/server/proxy.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_web $(TEST_WEB_SRCS) -lm -pthread -ldl

TEST_MAIL_SRCS = tests/test_mail.c src/server/server.c src/server/json.c src/server/mysql_wire.c src/server/fastcgi.c src/server/proxy.c src/server/mail.c src/server/rewrite.c src/server/tls.c src/server/h2.c src/widgets/png_write.c src/widgets/sql_text.c $(ENGINE_SRCS)

# cattura delle email: SMTP, MIME, pagine e API, sendmail, mail() di PHP
test_mail: $(TEST_MAIL_SRCS) src/server/server.h src/server/mail.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_mail $(TEST_MAIL_SRCS) -lm -pthread -ldl

TEST_TLS_SRCS = tests/test_tls.c src/server/server.c src/server/json.c src/server/mysql_wire.c src/server/fastcgi.c src/server/proxy.c src/server/mail.c src/server/rewrite.c src/server/tls.c src/server/h2.c src/widgets/png_write.c src/widgets/sql_text.c $(ENGINE_SRCS)

# HTTPS e HTTP/2 (client: curl; saltato senza curl o OpenSSL)
test_tls: $(TEST_TLS_SRCS) src/server/server.h src/server/tls.h src/server/h2.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_tls $(TEST_TLS_SRCS) -lm -pthread -ldl

TEST_PHP_SRCS = tests/test_php.c src/server/server.c src/server/json.c src/server/mysql_wire.c src/server/fastcgi.c src/server/proxy.c src/server/mail.c src/server/rewrite.c src/server/tls.c src/server/h2.c src/widgets/png_write.c src/widgets/sql_text.c $(ENGINE_SRCS)

# PHP via FastCGI: il test fa anche da php-cgi finto (e prova quello vero se è installato)
test_php: $(TEST_PHP_SRCS) src/server/server.h src/server/fastcgi.h src/server/json.h
	$(CC) $(CFLAGS) -I. $(BASE_INCLUDES) -o test_php $(TEST_PHP_SRCS) -lm -pthread -ldl

# gli script di examples/ eseguiti come nell'editor
test_examples: $(EXAMPLES_TEST_SRCS) $(wildcard examples/*.sql)
	$(CC) $(CFLAGS) -I. $(INCLUDES) -o test_examples $(EXAMPLES_TEST_SRCS) -lm -pthread

# compatibilità con il dialetto di MySQL (applicazioni PHP: WordPress, Laravel, ...)
test_mysql: tests/test_mysql.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -I. $(INCLUDES) -o test_mysql tests/test_mysql.c $(ENGINE_SRCS) -lm -pthread

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
		-I. $(BASE_INCLUDES) -o test_server_asan $(TEST_SERVER_SRCS) -lm -pthread -ldl
	ASAN_OPTIONS=detect_leaks=1 ./test_server_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_php_asan $(TEST_PHP_SRCS) -lm -pthread -ldl
	ASAN_OPTIONS=detect_leaks=1 ./test_php_asan
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
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_sessions_asan $(TEST_SESS_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_sessions_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_parallel_asan $(TEST_PAR_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_parallel_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_mysql_import_asan $(TEST_MY_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_mysql_import_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_overflow_asan $(TEST_OVF_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_overflow_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_backup_asan $(TEST_BK_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_backup_asan
	$(CC) -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I. $(BASE_INCLUDES) -o test_export_asan $(TEST_EXP_SRCS) -lm -pthread
	ASAN_OPTIONS=detect_leaks=1 ./test_export_asan

test_engine: test_engine.c $(ENGINE_SRCS) src/widgets/csv_import.c
	$(CC) $(CFLAGS) $(INCLUDES) -o test_engine test_engine.c $(ENGINE_SRCS) src/widgets/csv_import.c -lm -pthread

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# ── Cross-compilazione Windows da Linux (MinGW-w64) ──
#   make windows  ->  segvault.exe (statico: basta copiarlo con la cartella assets/)
WIN_CC   ?= x86_64-w64-mingw32-gcc
WIN_SRCS  = src/main.c src/platform/win32.c src/platform/fonts_embed.c $(WIDGET_SRCS) $(NET_SRCS) $(ENGINE_SRCS)
WIN_OBJS  = $(patsubst %.c,build-win/%.o,$(WIN_SRCS))

WIN_SERVER_OBJS = $(patsubst %.c,build-win/%.o,$(SERVER_SRCS))

windows: $(WIN_OBJS) $(WIN_SERVER_OBJS)
	$(WIN_CC) $(CFLAGS) -o segvault.exe $(WIN_OBJS) -static -mwindows -lgdi32 -luser32 -lkernel32 -lws2_32 -lshell32 -liphlpapi
	$(WIN_CC) $(CFLAGS) -o segvault-server.exe $(WIN_SERVER_OBJS) -static -lws2_32 -liphlpapi
	@echo "=== Windows build: segvault.exe, segvault-server.exe ==="

build-win/%.o: %.c
	@mkdir -p $(dir $@)
	$(WIN_CC) $(CFLAGS) $(DEPFLAGS) $(BASE_INCLUDES) -c $< -o $@

clean:
	$(RM) $(FULL_OBJS) $(FULL_OBJS:.o=.d) $(OUT) test_engine test_engine_asan test_btree test_btree_asan test_sql test_sql_asan bench_sql test_examples test_examples_asan
	$(RM) segvault-server segvault-server.exe libsegvault.a test_server test_server_asan test_web test_mail test_tls test_mysql test_php test_php_asan test_php_tsan test_grid_edit test_grid_edit_asan test_table_design test_table_design_asan test_schema_model test_schema_model_asan test_db_tools test_db_tools_asan test_png test_png_asan test_auth test_auth_asan test_sessions test_sessions_asan test_parallel test_parallel_asan test_parallel_tsan test_mysql_import test_mysql_import_asan test_overflow test_overflow_asan test_backup test_backup_asan test_export test_export_asan
	$(RM) -r obj build-win build-server build-dist

-include $(FULL_OBJS:.o=.d) $(WIN_OBJS:.o=.d) $(SERVER_OBJS:.o=.d) $(WIN_SERVER_OBJS:.o=.d)

# ── benchmark del motore (schema universitario, righe configurabili: ./bench_sql 100000) ──
bench_sql: bench/bench.c $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -I. $(INCLUDES) -o bench_sql bench/bench.c $(ENGINE_SRCS) -lm -pthread

bench: bench_sql
	./bench_sql 100000

# i font sono dentro l'eseguibile (.incbin): si ricompila se cambiano
src/platform/fonts_embed.o build-win/src/platform/fonts_embed.o: $(wildcard assets/fonts/*.ttf)

# ── eseguibili pronti da scaricare, nel repository (bin/): make dist ──
# Linux: l'app desktop e il server usano le librerie del sistema (glibc 2.38+; HTTPS carica OpenSSL
# all'avvio, cosa che un eseguibile statico non può fare); segvault-server-static va su ogni Linux
# x86-64, senza HTTPS. Windows 10 / 11: nessuna DLL da installare (HTTPS usa quelle di PHP).
# I font sono dentro gli eseguibili. Pacchetti portabili: bin/SegVault-VERSIONE-*.zip / .tar.gz
WIN_STRIP ?= x86_64-w64-mingw32-strip
VERSION := $(shell sed -n 's/.*SV_VERSION_STR *"\(.*\)".*/\1/p' include/common.h)
PKG_WIN := SegVault-$(VERSION)-windows-x86_64
PKG_LIN := SegVault-$(VERSION)-linux-x86_64
dist: all windows
	@mkdir -p bin/linux-x86_64 bin/windows-x86_64
	strip -o bin/linux-x86_64/segvault segvault
	strip -o bin/linux-x86_64/segvault-server segvault-server
	$(CC) -O2 -static -o bin/linux-x86_64/segvault-server-static $(SERVER_OBJS) -lm -pthread -ldl 2>/dev/null
	strip bin/linux-x86_64/segvault-server-static
	$(WIN_STRIP) -o bin/windows-x86_64/segvault.exe segvault.exe
	$(WIN_STRIP) -o bin/windows-x86_64/segvault-server.exe segvault-server.exe
	rm -rf build-dist
	mkdir -p build-dist/$(PKG_WIN)/php build-dist/$(PKG_LIN)
	cp bin/windows-x86_64/segvault.exe bin/windows-x86_64/segvault-server.exe build-dist/$(PKG_WIN)/
	for f in packaging/windows/*.cmd packaging/README.txt; do sed 's/$$/\r/' "$$f" > build-dist/$(PKG_WIN)/$$(basename "$$f"); done
	printf 'Run get-php.cmd (next to segvault.exe) to download PHP here, or extract any PHP for Windows (Non Thread Safe, x64) into this folder.\r\n' > build-dist/$(PKG_WIN)/php/README.txt
	cp -r packaging/www build-dist/$(PKG_WIN)/www
	cp bin/linux-x86_64/segvault bin/linux-x86_64/segvault-server packaging/linux/start-server.sh packaging/README.txt build-dist/$(PKG_LIN)/
	cp -r packaging/www build-dist/$(PKG_LIN)/www
	cd build-dist && bsdtar -a -cf ../bin/$(PKG_WIN).zip $(PKG_WIN) && tar -czf ../bin/$(PKG_LIN).tar.gz $(PKG_LIN)
	@echo "=== bin/: eseguibili pronti, $(PKG_WIN).zip, $(PKG_LIN).tar.gz ==="
