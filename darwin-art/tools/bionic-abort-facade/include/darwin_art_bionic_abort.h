#ifndef DARWIN_ART_BIONIC_ABORT_H_
#define DARWIN_ART_BIONIC_ABORT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DarwinArtBionicAbortMessage {
  size_t size;
  char message[];
} DarwinArtBionicAbortMessage;

__attribute__((noreturn)) void darwin_art_bionic_abort(void);
void darwin_art_bionic_android_set_abort_message(const char* message);

void* darwin_art_bionic_abort_resolve(const char* soname,
                                      const char* symbol,
                                      const char* version);
int darwin_art_bionic_abort_capability(const char* capability);

/* Immutable process-lifetime inspection seam for conformance probes. */
const DarwinArtBionicAbortMessage*
darwin_art_bionic_abort_message_for_test(void);

#ifdef __cplusplus
}
#endif

#endif  // DARWIN_ART_BIONIC_ABORT_H_
