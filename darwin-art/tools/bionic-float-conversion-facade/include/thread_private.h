#ifndef DARWIN_ART_GDTOA_THREAD_PRIVATE_H_
#define DARWIN_ART_GDTOA_THREAD_PRIVATE_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DARWIN_ART_GDTOA_LOCAL_LOCKS
extern void* __dtoa_locks[];
#endif
void darwin_art_gdtoa_lock(void* identity);
void darwin_art_gdtoa_unlock(void* identity);

#ifdef __cplusplus
}
#endif

#define _MUTEX_LOCK(lock) darwin_art_gdtoa_lock((void*)(lock))
#define _MUTEX_UNLOCK(lock) darwin_art_gdtoa_unlock((void*)(lock))

#endif  // DARWIN_ART_GDTOA_THREAD_PRIVATE_H_
