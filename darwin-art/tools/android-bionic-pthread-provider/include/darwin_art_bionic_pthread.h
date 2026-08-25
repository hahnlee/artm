#ifndef DARWIN_ART_BIONIC_PTHREAD_H_
#define DARWIN_ART_BIONIC_PTHREAD_H_

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Android arm64 NDK object representations. They are transport storage only;
// the provider never casts one to a Darwin pthread object.
typedef int64_t DarwinArtAndroidPthread;
typedef int32_t DarwinArtAndroidPthreadKey;
typedef int32_t DarwinArtAndroidPthreadOnce;
typedef int64_t DarwinArtAndroidPthreadMutexAttr;
typedef int64_t DarwinArtAndroidPthreadRwlockAttr;
typedef struct DarwinArtAndroidPthreadAttr {
  uint32_t flags;
  void* stack_base;
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
  unsigned char reserved[16];
} DarwinArtAndroidPthreadAttr;
typedef struct DarwinArtAndroidPthreadMutex {
  int32_t opaque[10];
} DarwinArtAndroidPthreadMutex;
typedef struct DarwinArtAndroidPthreadCond {
  int32_t opaque[12];
} DarwinArtAndroidPthreadCond;
typedef struct DarwinArtAndroidPthreadRwlock {
  int32_t opaque[14];
} DarwinArtAndroidPthreadRwlock;

typedef void (*DarwinArtAndroidTlsDestructor)(void* value);
typedef void (*DarwinArtAndroidOnceRoutine)(void);
typedef void* (*DarwinArtAndroidThreadRoutine)(void* argument);

DarwinArtAndroidPthread darwin_art_bionic_pthread_self(void);
// Coherent lifecycle owner seam. Only null attributes are currently accepted;
// returned tokens are the sole valid inputs to join/detach.
int darwin_art_bionic_pthread_create(
    DarwinArtAndroidPthread* thread,
    const void* android_attributes,
    DarwinArtAndroidThreadRoutine routine,
    void* argument);
int darwin_art_bionic_pthread_join(DarwinArtAndroidPthread thread,
                                   void** return_value);
int darwin_art_bionic_pthread_detach(DarwinArtAndroidPthread thread);
int darwin_art_bionic_pthread_attr_init(DarwinArtAndroidPthreadAttr* attr);
int darwin_art_bionic_pthread_attr_destroy(DarwinArtAndroidPthreadAttr* attr);
int darwin_art_bionic_pthread_key_create(
    DarwinArtAndroidPthreadKey* key,
    DarwinArtAndroidTlsDestructor destructor);
int darwin_art_bionic_pthread_key_delete(DarwinArtAndroidPthreadKey key);
void* darwin_art_bionic_pthread_getspecific(DarwinArtAndroidPthreadKey key);
int darwin_art_bionic_pthread_setspecific(DarwinArtAndroidPthreadKey key,
                                          const void* value);
int darwin_art_bionic_pthread_once(DarwinArtAndroidPthreadOnce* once,
                                   DarwinArtAndroidOnceRoutine routine);
int darwin_art_bionic_pthread_mutexattr_init(
    DarwinArtAndroidPthreadMutexAttr* attributes);
int darwin_art_bionic_pthread_mutexattr_destroy(
    DarwinArtAndroidPthreadMutexAttr* attributes);
int darwin_art_bionic_pthread_mutexattr_settype(
    DarwinArtAndroidPthreadMutexAttr* attributes,
    int type);
int darwin_art_bionic_pthread_mutex_init(DarwinArtAndroidPthreadMutex* mutex,
    const DarwinArtAndroidPthreadMutexAttr* android_attributes);
int darwin_art_bionic_pthread_mutex_lock(DarwinArtAndroidPthreadMutex* mutex);
int darwin_art_bionic_pthread_mutex_trylock(DarwinArtAndroidPthreadMutex* mutex);
int darwin_art_bionic_pthread_mutex_unlock(DarwinArtAndroidPthreadMutex* mutex);
int darwin_art_bionic_pthread_mutex_destroy(DarwinArtAndroidPthreadMutex* mutex);
int darwin_art_bionic_pthread_cond_wait(DarwinArtAndroidPthreadCond* cond,
                                        DarwinArtAndroidPthreadMutex* mutex);
int darwin_art_bionic_pthread_cond_timedwait(
    DarwinArtAndroidPthreadCond* cond,
    DarwinArtAndroidPthreadMutex* mutex,
    const struct timespec* absolute_timeout);
int darwin_art_bionic_pthread_cond_signal(DarwinArtAndroidPthreadCond* cond);
int darwin_art_bionic_pthread_cond_broadcast(DarwinArtAndroidPthreadCond* cond);
int darwin_art_bionic_pthread_cond_destroy(DarwinArtAndroidPthreadCond* cond);
int darwin_art_bionic_pthread_rwlock_rdlock(
    DarwinArtAndroidPthreadRwlock* rwlock);
int darwin_art_bionic_pthread_rwlock_wrlock(
    DarwinArtAndroidPthreadRwlock* rwlock);
int darwin_art_bionic_pthread_rwlock_unlock(
    DarwinArtAndroidPthreadRwlock* rwlock);
int darwin_art_bionic_pthread_rwlock_init(
    DarwinArtAndroidPthreadRwlock* rwlock,
    const DarwinArtAndroidPthreadRwlockAttr* attributes);
int darwin_art_bionic_pthread_rwlock_destroy(
    DarwinArtAndroidPthreadRwlock* rwlock);

// Closed libc.so/LIBC resolver. Unknown SONAME, version, or symbol is rejected.
void* darwin_art_bionic_pthread_resolve(const char* soname,
                                        const char* symbol,
                                        const char* version);

// Test/process-shutdown boundary. It succeeds only when no owned thread,
// TLS-key, or live synchronization object remains; once entries may be
// discarded after all consumers stop.
int darwin_art_bionic_pthread_provider_reset(void);
size_t darwin_art_bionic_pthread_provider_retired_cell_count(void);

// Returns 1 only for implemented capability names. Unsupported names are
// explicit and never forwarded to a Darwin symbol with a similar spelling.
int darwin_art_bionic_pthread_capability(const char* capability);

#ifdef __cplusplus
}
#endif

#endif  // DARWIN_ART_BIONIC_PTHREAD_H_
