#ifndef DARWIN_ART_BIONIC_SYSLOG_H_
#define DARWIN_ART_BIONIC_SYSLOG_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DarwinArtBionicSyslogFunction)(void);

void darwin_art_bionic_openlog(const char* ident, int option, int facility);
void darwin_art_bionic_closelog(void);
void darwin_art_bionic_syslog(int priority, const char* format, ...);

/* One-time owned copy of the guest process tag used when openlog has no tag. */
int darwin_art_bionic_syslog_activate(const char* guest_program_tag);

DarwinArtBionicSyslogFunction darwin_art_bionic_syslog_resolve(
    const char* soname, const char* symbol, const char* version);
const char* darwin_art_bionic_syslog_capability(const char* capability);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARWIN_ART_BIONIC_SYSLOG_H_
