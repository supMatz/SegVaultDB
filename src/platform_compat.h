#ifndef PLATFORM_COMPAT_H
#define PLATFORM_COMPAT_H

#include "common.h"

#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
  #include <io.h>
  #include <direct.h>
  #include <fcntl.h>

  #ifdef _MSC_VER
    #include <BaseTsd.h>
    typedef SSIZE_T ssize_t;
    typedef long off_t;
  #endif

  #define SV_PATH_SEP "\\"

  static inline int sv_open(const char* path, int flags, int mode) {
      (void)mode;
      return _open(path, flags | _O_BINARY);
  }
  static inline int sv_close(int fd) { return _close(fd); }
  static inline int sv_read(int fd, void* buf, unsigned int count) { return _read(fd, buf, count); }
  static inline int sv_write(int fd, const void* buf, unsigned int count) { return _write(fd, buf, count); }
  static inline long sv_lseek(int fd, long offset, int whence) { return _lseek(fd, offset, whence); }

  static inline int sv_fsync(int fd) { return _commit(fd); }

  static inline int sv_ftruncate(int fd, long length) { return _chsize_s(fd, length); }

  static inline int sv_mkdir(const char* path) { return _mkdir(path); }

  static inline void sv_sleep_ms(int ms) {
    extern void __stdcall Sleep(unsigned long);
    Sleep((unsigned long)ms);
  }

  static inline double sv_get_time_ms(void) {
    extern unsigned long long __stdcall GetTickCount64(void);
    return (double)GetTickCount64();
  }

  static inline ssize_t sv_pread(int fd, void* buf, size_t count, off_t offset) {
      long cur = _lseek(fd, 0, SEEK_CUR);
      if (cur < 0) return -1;
      if (_lseek(fd, offset, SEEK_SET) < 0) return -1;
      int n = _read(fd, buf, (unsigned int)count);
      _lseek(fd, cur, SEEK_SET);
      return (ssize_t)n;
  }

  static inline ssize_t sv_pwrite(int fd, const void* buf, size_t count, off_t offset) {
      long cur = _lseek(fd, 0, SEEK_CUR);
      if (cur < 0) return -1;
      if (_lseek(fd, offset, SEEK_SET) < 0) return -1;
      int n = _write(fd, buf, (unsigned int)count);
      _lseek(fd, cur, SEEK_SET);
      return (ssize_t)n;
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

  #define sv_open(path, flags, mode)    open(path, flags, mode)
  #define sv_close(fd)                  close(fd)
  #define sv_read(fd, buf, count)       read(fd, buf, count)
  #define sv_write(fd, buf, count)      write(fd, buf, count)
  #define sv_lseek(fd, off, whence)     lseek(fd, off, whence)
  #define sv_fsync(fd)                  fsync(fd)
  #define sv_ftruncate(fd, len)         ftruncate(fd, len)
  #define sv_mkdir(path)                mkdir(path, 0755)
  #define sv_sleep_ms(ms)               usleep((ms) * 1000)
  #define sv_pread(fd, buf, cnt, off)   pread(fd, buf, cnt, off)
  #define sv_pwrite(fd, buf, cnt, off)  pwrite(fd, buf, cnt, off)

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

#endif
