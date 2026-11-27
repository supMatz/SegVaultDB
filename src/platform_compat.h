#ifndef PLATFORM_COMPAT_H
#define PLATFORM_COMPAT_H

#include "common.h"
#include <stdint.h>

#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
  #include <io.h>
  #include <direct.h>
  #include <fcntl.h>
  #include <sys/stat.h>

  #ifdef _MSC_VER
    #include <BaseTsd.h>
    typedef SSIZE_T ssize_t;
    typedef long off_t;
  #endif

  #define SV_PATH_SEP "\\"

  static inline int sv_open(const char* path, int flags, int mode) {
      (void)mode;
      // con _O_CREAT il terzo argomento (permessi) è obbligatorio: senza, il file
      // può essere creato in sola lettura e le aperture successive in scrittura falliscono
      // _O_NOINHERIT: i programmi CGI (avviati ereditando gli handle per le pipe) non ricevono i file del motore
      return _open(path, flags | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
  }
  static inline int sv_close(int fd) { return _close(fd); }
  static inline int sv_read(int fd, void* buf, unsigned int count) { return _read(fd, buf, count); }
  static inline int sv_write(int fd, const void* buf, unsigned int count) { return _write(fd, buf, count); }
  // offset a 64 bit: long e off_t sono a 32 bit con MinGW, oltre 2 GiB (pagina 524 288) si sbagliava posizione
  static inline int64_t sv_lseek(int fd, int64_t offset, int whence) { return _lseeki64(fd, offset, whence); }

  static inline int sv_fsync(int fd) { return _commit(fd); }

  static inline int sv_ftruncate(int fd, int64_t length) { return _chsize_s(fd, length) == 0 ? 0 : -1; }

  static inline int sv_mkdir(const char* path) { return _mkdir(path); }

  static inline void sv_sleep_ms(int ms) {
#ifndef _WINBASE_                       // windows.h già incluso: le sue dichiarazioni (dllimport)
    extern void __stdcall Sleep(unsigned long);
#endif
    Sleep((unsigned long)ms);
  }

  static inline double sv_get_time_ms(void) {
#ifndef _WINBASE_
    extern unsigned long long __stdcall GetTickCount64(void);
#endif
    return (double)GetTickCount64();
  }

  // lettura / scrittura posizionali vere (ReadFile / WriteFile con OVERLAPPED): offset a 64 bit, e la
  // posizione corrente del descrittore non cambia (niente seek + read + seek, che non era atomico).
  // Dichiarazioni con gli stessi tipi di <windows.h>, senza includerlo qui
  struct _OVERLAPPED;
  typedef struct { uintptr_t Internal, InternalHigh; unsigned long Offset, OffsetHigh; void* hEvent; } SvOverlapped;
  __declspec(dllimport) int __stdcall ReadFile(void*, void*, unsigned long, unsigned long*, struct _OVERLAPPED*);
  __declspec(dllimport) int __stdcall WriteFile(void*, const void*, unsigned long, unsigned long*, struct _OVERLAPPED*);
  __declspec(dllimport) unsigned long __stdcall GetLastError(void);

  static inline ssize_t sv_pread(int fd, void* buf, size_t count, int64_t offset) {
      void* h = (void*)_get_osfhandle(fd);
      if (h == (void*)(intptr_t)-1) return -1;
      SvOverlapped ov = { 0, 0, (unsigned long)((uint64_t)offset & 0xFFFFFFFFu), (unsigned long)((uint64_t)offset >> 32), NULL };
      unsigned long got = 0;
      if (!ReadFile(h, buf, (unsigned long)count, &got, (struct _OVERLAPPED*)&ov))
          return GetLastError() == 38 /* ERROR_HANDLE_EOF */ ? 0 : -1;
      return (ssize_t)got;
  }

  static inline ssize_t sv_pwrite(int fd, const void* buf, size_t count, int64_t offset) {
      void* h = (void*)_get_osfhandle(fd);
      if (h == (void*)(intptr_t)-1) return -1;
      SvOverlapped ov = { 0, 0, (unsigned long)((uint64_t)offset & 0xFFFFFFFFu), (unsigned long)((uint64_t)offset >> 32), NULL };
      unsigned long put = 0;
      if (!WriteFile(h, buf, (unsigned long)count, &put, (struct _OVERLAPPED*)&ov)) return -1;
      return (ssize_t)put;
  }

  #define SV_O_RDONLY  _O_RDONLY
  #define SV_O_WRONLY  _O_WRONLY
  #define SV_O_RDWR    _O_RDWR
  #define SV_O_CREAT   _O_CREAT
  #define SV_O_TRUNC   _O_TRUNC
  #define SV_O_APPEND  _O_APPEND

#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <time.h>

  #define SV_PATH_SEP "/"

  #ifdef O_CLOEXEC                      // nessun descrittore del motore passa ai processi figli (P2.10)
    #define sv_open(path, flags, mode)  open(path, (flags) | O_CLOEXEC, mode)
  #else
    #define sv_open(path, flags, mode)  open(path, flags, mode)
  #endif
  #define sv_close(fd)                  close(fd)
  #define sv_read(fd, buf, count)       read(fd, buf, count)
  #define sv_write(fd, buf, count)      write(fd, buf, count)
  #define sv_lseek(fd, off, whence)     lseek(fd, off, whence)
  #define sv_fsync(fd)                  fsync(fd)
  #define sv_ftruncate(fd, len)         ftruncate(fd, len)
  #define sv_mkdir(path)                mkdir(path, 0700)   // cartelle dei dati e dei backup: solo il proprietario
  #define sv_sleep_ms(ms)               usleep((ms) * 1000)
  #define sv_pread(fd, buf, cnt, off)   pread(fd, buf, cnt, off)
  #define sv_pwrite(fd, buf, cnt, off)  pwrite(fd, buf, cnt, off)

  // build di prova con -DSV_FAULT_INJECTION: le scritture e gli fsync passano da funzioni del test,
  // che possono farli fallire o fermare il processo all'N-esima chiamata (tests/test_durability.c)
  #ifdef SV_FAULT_INJECTION
    ssize_t sv_fi_write(int fd, const void* buf, size_t count);
    ssize_t sv_fi_pwrite(int fd, const void* buf, size_t count, off_t off);
    int     sv_fi_fsync(int fd);
    ssize_t sv_fi_pread(int fd, void* buf, size_t count, off_t off);
    #undef  sv_pread
    #define sv_pread(fd, buf, cnt, off)   sv_fi_pread(fd, buf, cnt, off)
    #undef  sv_write
    #undef  sv_pwrite
    #undef  sv_fsync
    #define sv_write(fd, buf, count)      sv_fi_write(fd, buf, count)
    #define sv_pwrite(fd, buf, cnt, off)  sv_fi_pwrite(fd, buf, cnt, off)
    #define sv_fsync(fd)                  sv_fi_fsync(fd)
  #endif

  #define SV_O_RDONLY  O_RDONLY
  #define SV_O_WRONLY  O_WRONLY
  #define SV_O_RDWR    O_RDWR
  #define SV_O_CREAT   O_CREAT
  #define SV_O_TRUNC   O_TRUNC
  #define SV_O_APPEND  O_APPEND

  static inline double sv_get_time_ms(void) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
  }
#endif


// sostituisce path con tmp (già scritto e con fsync): atomico. Su Windows MoveFileEx con
// REPLACE_EXISTING | WRITE_THROUGH (remove + rename lasciava un istante senza file); su POSIX rename e
// poi fsync della cartella, così la sostituzione stessa sopravvive a una caduta di corrente
#ifdef _WIN32
static inline int sv_replace_file(const char* tmp, const char* path) {
#ifndef _WINBASE_
    extern int __stdcall MoveFileExA(const char*, const char*, unsigned long);
#endif
    return MoveFileExA(tmp, path, 0x1 /* REPLACE_EXISTING */ | 0x8 /* WRITE_THROUGH */) ? 0 : -1;
}
#else
#include <stdio.h>
#include <string.h>
static inline int sv_replace_file(const char* tmp, const char* path) {
    if (rename(tmp, path) != 0) return -1;
    char dir[4096];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)) return 0;   // (sostituito; niente fsync della cartella)
    char* sl = strrchr(dir, '/');
    if (sl) { if (sl == dir) sl[1] = '\0'; else *sl = '\0'; } else snprintf(dir, sizeof(dir), ".");
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
    return 0;
}
#endif

// chiude f (aperto su tmp e scritto) con fflush + fsync e lo mette al posto di path: chi legge trova il file
// vecchio o quello nuovo intero, mai uno troncato da una caduta a metà. false (tmp rimosso) se qualcosa non va
static inline bool sv_fclose_replace(FILE* f, const char* tmp, const char* path) {
    bool ok = fflush(f) == 0 && !ferror(f) && sv_fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = sv_replace_file(tmp, path) == 0;
    if (!ok) remove(tmp);
    return ok;
}

// file del motore leggibili solo dal proprietario (dati, log, catalogo, utenti con gli hash delle password):
// su POSIX creati con permessi 0600; mode "w" o "a" come fopen
#ifdef _WIN32
int sv_win_private(const char* path, int is_dir);      // ACL protetta: utente corrente + SYSTEM (win_acl.c)
void sv_win_acl_sddl(const char* path, char* out, size_t n);   // per i test
int  sv_win_under_wine(void);
int  sv_win_trusted_path(const char* path);   // file e cartelle di proprietà fidata (CR-M8, win_acl.c)
static inline FILE* sv_fopen_private(const char* path, const char* mode) {
    FILE* f = fopen(path, mode);
    if (f) sv_win_private(path, 0);
    return f;
}
#else
static inline FILE* sv_fopen_private(const char* path, const char* mode) {
    int fd = open(path, O_WRONLY | O_CREAT | (mode[0] == 'a' ? O_APPEND : O_TRUNC), 0600);
    if (fd < 0) return NULL;
    FILE* f = fdopen(fd, mode);
    if (!f) close(fd);
    return f;
}
#endif

// posizioni nei file: sempre a 64 bit (file oltre 2 GiB)
#ifndef _WIN32
_Static_assert(sizeof(off_t) == 8, "build with -D_FILE_OFFSET_BITS=64: off_t must be 64-bit");
#endif

// lettura / scrittura complete (read e write possono trasferire meno byte del richiesto);
// false su errore o fine file prima di count byte
static inline bool sv_read_full(int fd, void* buf, size_t count) {
    unsigned char* p = (unsigned char*)buf;
    while (count > 0) {
        unsigned int chunk = count > (1u << 24) ? (1u << 24) : (unsigned int)count;
        long r = (long)sv_read(fd, p, chunk);
        if (r <= 0) return false;
        p += r;
        count -= (size_t)r;
    }
    return true;
}
static inline bool sv_write_full(int fd, const void* buf, size_t count) {
    const unsigned char* p = (const unsigned char*)buf;
    while (count > 0) {
        unsigned int chunk = count > (1u << 24) ? (1u << 24) : (unsigned int)count;
        long w = (long)sv_write(fd, p, chunk);
        if (w <= 0) return false;
        p += w;
        count -= (size_t)w;
    }
    return true;
}

#endif
