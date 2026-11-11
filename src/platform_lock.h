/*
SCOPO: lock lettori/scrittori portabile per il motore (SRWLOCK su Windows, pthread_rwlock altrove).
       Non ricorsivo: chi lo usa in modo annidato tiene un contatore per thread (vedi db_api.c).
       Usato anche come semplice mutex (solo sv_rw_wr / sv_rw_wrunlock).
*/
#ifndef SV_PLATFORM_LOCK_H
#define SV_PLATFORM_LOCK_H

#ifdef _WIN32
  #if !defined(_WINDOWS_)
    __declspec(dllimport) void __stdcall AcquireSRWLockExclusive(void** lock);
    __declspec(dllimport) void __stdcall ReleaseSRWLockExclusive(void** lock);
    __declspec(dllimport) void __stdcall AcquireSRWLockShared(void** lock);
    __declspec(dllimport) void __stdcall ReleaseSRWLockShared(void** lock);
    typedef struct { void* srw; } SvRwLock;           // SRWLOCK: un puntatore, inizializzato a zero
    #define SV_RW_LOCKP(l) (&(l)->srw)
  #else
    typedef struct { SRWLOCK srw; } SvRwLock;
    #define SV_RW_LOCKP(l) (&(l)->srw)
  #endif
  #define SV_RWLOCK_INIT { 0 }
  static inline void sv_rw_init(SvRwLock* l)     { l->srw = 0; }
  static inline void sv_rw_rd(SvRwLock* l)       { AcquireSRWLockShared(SV_RW_LOCKP(l)); }
  static inline void sv_rw_rdunlock(SvRwLock* l) { ReleaseSRWLockShared(SV_RW_LOCKP(l)); }
  static inline void sv_rw_wr(SvRwLock* l)       { AcquireSRWLockExclusive(SV_RW_LOCKP(l)); }
  static inline void sv_rw_wrunlock(SvRwLock* l) { ReleaseSRWLockExclusive(SV_RW_LOCKP(l)); }
#else
  #include <pthread.h>
  typedef struct { pthread_rwlock_t rw; } SvRwLock;
  #define SV_RWLOCK_INIT { PTHREAD_RWLOCK_INITIALIZER }
  // gli scrittori hanno la precedenza: un flusso continuo di letture non li blocca per sempre
  static inline void sv_rw_init(SvRwLock* l) {
      pthread_rwlockattr_t a;
      pthread_rwlockattr_init(&a);
  #ifdef __GLIBC__
      pthread_rwlockattr_setkind_np(&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
  #endif
      pthread_rwlock_init(&l->rw, &a);
      pthread_rwlockattr_destroy(&a);
  }
  static inline void sv_rw_rd(SvRwLock* l)       { pthread_rwlock_rdlock(&l->rw); }
  static inline void sv_rw_rdunlock(SvRwLock* l) { pthread_rwlock_unlock(&l->rw); }
  static inline void sv_rw_wr(SvRwLock* l)       { pthread_rwlock_wrlock(&l->rw); }
  static inline void sv_rw_wrunlock(SvRwLock* l) { pthread_rwlock_unlock(&l->rw); }
#endif

#endif
