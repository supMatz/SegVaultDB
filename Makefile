ifeq ($(OS), Windows_NT)
    # ── Windows ──
    PLATFORM_SRC = src/platform/win32.c
    LIBS         = -lgdi32 -luser32 -lkernel32
    OUT          = segvault.exe
    MKDIR        = mkdir
    RM           = del /Q /F
    SEP          = \\
else
    # ── Linux / macOS ──
    PLATFORM_SRC = src/platform/xlib.c
    LIBS         = -lX11 -lm
    OUT          = segvault
    MKDIR        = mkdir -p
    RM           = rm -f
    SEP          = /
endif