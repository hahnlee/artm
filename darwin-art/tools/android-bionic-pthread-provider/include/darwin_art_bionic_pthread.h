#ifndef DARWIN_ART_BIONIC_PTHREAD_H_
#define DARWIN_ART_BIONIC_PTHREAD_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Android arm64 NDK object representations. They are transport storage only;
// the provider never casts one to a Darwin pthread object.
typedef int64_t DarwinArtAndroidPthread;
typedef int32_t DarwinArtAndroidPthreadKey;
typedef int32_t DarwinArtAndroidPthreadOnce;
typedef struct DarwinArtAndroidPthreadMutex {
  int32_t opaque[10];
} DarwinArtAndroidPthreadMutex;

typedef void (*DarwinArtAndroidTlsDestructor)(void* value);
typedef void (*DarwinArtAndroidOnceRoutine)(void);

DarwinArtAndroidPthread darwin_art_bionic_pthread_self(void);
int darwin_art_bionic_pthread_key_create(
    DarwinArtAndroidPthreadKey* key,
    DarwinArtAndroidTlsDestructor destructor);
int darwin_art_bionic_pthread_key_delete(DarwinArtAndroidPthreadKey key);
void* darwin_art_bionic_pthread_getspecific(DarwinArtAndroidPthreadKey key);
int darwin_art_bionic_pthread_setspecific(DarwinArtAndroidPthreadKey key,
                                          const void* value);
int darwin_art_bionic_pthread_once(DarwinArtAndroidPthreadOnce* once,
                                   DarwinArtAndroidOnceRoutine routine);
int darwin_art_bionic_pthread_mutex_init(DarwinArtAndroidPthreadMutex* mutex,
                                         const void* android_attributes);
int darwin_art_bionic_pthread_mutex_lock(DarwinArtAndroidPthreadMutex* mutex);
int darwin_art_bionic_pthread_mutex_trylock(DarwinArtAndroidPthreadMutex* mutex);
int darwin_art_bionic_pthread_mutex_unlock(DarwinArtAndroidPthreadMutex* mutex);
int darwin_art_bionic_pthread_mutex_destroy(DarwinArtAndroidPthreadMutex* mutex);

// Closed libc.so/LIBC resolver. Unknown SONAME, version, or symbol is rejected.
void* darwin_art_bionic_pthread_resolve(const char* soname,
                                        const char* symbol,
                                        const char* version);

// Test/process-shutdown boundary. It succeeds only when no TLS keys or live
// mutexes remain; once entries may be discarded after all consumers stop.
int darwin_art_bionic_pthread_provider_reset(void);
size_t darwin_art_bionic_pthread_provider_retired_cell_count(void);

// Returns 1 only for implemented capability names. Unsupported names are
// explicit and never forwarded to a Darwin symbol with a similar spelling.
int darwin_art_bionic_pthread_capability(const char* capability);

#ifdef __cplusplus
}
#endif

#endif  // DARWIN_ART_BIONIC_PTHREAD_H_
