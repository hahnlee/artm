#include "darwin_art_bionic_stdio.h"
#include <errno.h>
#include <stddef.h>

extern int32_t darwin_art_bionic_errno_load(void);

DarwinArtAndroidFile darwin_art_bionic___sF[3];
DarwinArtAndroidFile* darwin_art_bionic_stdin = &darwin_art_bionic___sF[0];
DarwinArtAndroidFile* darwin_art_bionic_stdout = &darwin_art_bionic___sF[1];
DarwinArtAndroidFile* darwin_art_bionic_stderr = &darwin_art_bionic___sF[2];
_Static_assert(sizeof(DarwinArtAndroidFile) == 152, "Android FILE size drift");
_Static_assert(_Alignof(DarwinArtAndroidFile) == 8, "Android FILE align drift");

#define WRAP(saved, call) do { const int saved = errno; const __typeof__(call) result = (call); errno = saved; return result; } while (0)
DarwinArtAndroidFile* darwin_art_bionic_fopen(const char* p, const char* m) { WRAP(e, darwin_art_bionic_stdio_fopen_core(p,m)); }
int darwin_art_bionic_fclose(DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_fclose_core(f)); }
int darwin_art_bionic_fflush(DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_fflush_core(f)); }
int darwin_art_bionic_fileno(DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_fileno_core(f)); }
size_t darwin_art_bionic_fread(void* b,size_t s,size_t n,DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_fread_core(b,s,n,f)); }
size_t darwin_art_bionic_fwrite(const void* b,size_t s,size_t n,DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_fwrite_core(b,s,n,f)); }
int darwin_art_bionic_fseek(DarwinArtAndroidFile* f,long o,int w) { WRAP(e, darwin_art_bionic_stdio_fseek_core(f,o,w)); }
int darwin_art_bionic_fseeko(DarwinArtAndroidFile* f,int64_t o,int w) { WRAP(e, darwin_art_bionic_stdio_fseek_core(f,o,w)); }
int64_t darwin_art_bionic_ftello(DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_ftello_core(f)); }
int darwin_art_bionic_fputc(int c,DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_fputc_core(c,f)); }
int darwin_art_bionic_fputs(const char* s,DarwinArtAndroidFile* f) {
  if (s == NULL) return -1;
  size_t n = 0;
  const volatile char* cursor = s;
  while (*cursor++ != '\0') ++n;
  const int saved = errno;
  const size_t written = darwin_art_bionic_stdio_fwrite_core(s,1,n,f);
  errno = saved;
  return written == n ? 0 : -1;
}
int darwin_art_bionic_getc(DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_getc_core(f)); }
int darwin_art_bionic_ungetc(int c,DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_ungetc_core(c,f)); }
int darwin_art_bionic_feof(DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_feof_core(f)); }
int darwin_art_bionic_ferror(DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_ferror_core(f)); }
long darwin_art_bionic_ftell(DarwinArtAndroidFile* f) { WRAP(e, (long)darwin_art_bionic_stdio_ftello_core(f)); }
char* darwin_art_bionic_fgets(char* s,int n,DarwinArtAndroidFile* f) {
  if (s == NULL || n <= 0) return NULL;
  int i = 0;
  while (i + 1 < n) {
    int c = darwin_art_bionic_stdio_getc_core(f);
    if (c == -1) break;
    s[i++] = (char)c;
    if (c == '\n') break;
  }
  if (i == 0) return NULL;
  s[i] = '\0';
  return s;
}
int darwin_art_bionic_puts(const char* s) {
  if (s == NULL) return -1;
  if (darwin_art_bionic_fputs(s,darwin_art_bionic_stdout) < 0)
    return -1;
  return darwin_art_bionic_fputc('\n',darwin_art_bionic_stdout);
}
void darwin_art_bionic_setbuf(DarwinArtAndroidFile* f, char* buffer) {
  /* The compatibility stream owns its backing bytes and makes writes visible
   * immediately. Bionic setbuf(stream, NULL) therefore already has its exact
   * unbuffered effect; a caller buffer must never become host-owned storage. */
  (void)f;
  (void)buffer;
}
__attribute__((no_stack_protector)) static void FormatErrno(int32_t number,char output[32]) {
  static const char kPrefix[] = "Error ";
  size_t length = sizeof(kPrefix) - 1;
  for (size_t index = 0; index < length; ++index) output[index] = kPrefix[index];
  uint32_t magnitude;
  if (number < 0) {
    output[length++] = '-';
    magnitude = (uint32_t)(-(int64_t)number);
  } else {
    magnitude = (uint32_t)number;
  }
  char reverse[10];
  size_t digits = 0;
  do {
    reverse[digits++] = (char)('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  while (digits != 0) output[length++] = reverse[--digits];
  output[length] = '\0';
}
__attribute__((no_stack_protector)) void darwin_art_bionic_perror(const char* prefix) {
  const int saved = errno;
  char message[32];
  FormatErrno(darwin_art_bionic_errno_load(),message);
  if (prefix != NULL && prefix[0] != '\0') {
    (void)darwin_art_bionic_fputs(prefix,darwin_art_bionic_stderr);
    (void)darwin_art_bionic_fputs(": ",darwin_art_bionic_stderr);
  }
  (void)darwin_art_bionic_fputs(message,darwin_art_bionic_stderr);
  (void)darwin_art_bionic_fputc('\n',darwin_art_bionic_stderr);
  errno = saved;
}
int darwin_art_bionic_vprintf(const char* format, const void* ap) { (void)format; (void)ap; return 0; }

static int Compare(const char* a,const char* b) { while(*a==*b&&*a){a++;b++;} return (unsigned char)*a<(unsigned char)*b?-1:((unsigned char)*a!=(unsigned char)*b); }
typedef struct { const char* name; DarwinArtBionicStdioFunction address; } Binding;
static const Binding kBindings[] = {
 {"__sF",(DarwinArtBionicStdioFunction)darwin_art_bionic___sF},
 {"fclose",(DarwinArtBionicStdioFunction)darwin_art_bionic_fclose},
 {"feof",(DarwinArtBionicStdioFunction)darwin_art_bionic_feof},
 {"ferror",(DarwinArtBionicStdioFunction)darwin_art_bionic_ferror},
 {"fflush",(DarwinArtBionicStdioFunction)darwin_art_bionic_fflush},
 {"fgets",(DarwinArtBionicStdioFunction)darwin_art_bionic_fgets},
 {"fileno",(DarwinArtBionicStdioFunction)darwin_art_bionic_fileno},
 {"fopen",(DarwinArtBionicStdioFunction)darwin_art_bionic_fopen},
 {"fputc",(DarwinArtBionicStdioFunction)darwin_art_bionic_fputc},
 {"fputs",(DarwinArtBionicStdioFunction)darwin_art_bionic_fputs},
 {"fread",(DarwinArtBionicStdioFunction)darwin_art_bionic_fread},
 {"fseek",(DarwinArtBionicStdioFunction)darwin_art_bionic_fseek},
 {"fseeko",(DarwinArtBionicStdioFunction)darwin_art_bionic_fseeko},
 {"ftell",(DarwinArtBionicStdioFunction)darwin_art_bionic_ftell},
 {"ftello",(DarwinArtBionicStdioFunction)darwin_art_bionic_ftello},
 {"fwrite",(DarwinArtBionicStdioFunction)darwin_art_bionic_fwrite},
 {"getc",(DarwinArtBionicStdioFunction)darwin_art_bionic_getc},
 {"perror",(DarwinArtBionicStdioFunction)darwin_art_bionic_perror},
 {"puts",(DarwinArtBionicStdioFunction)darwin_art_bionic_puts},
 {"setbuf",(DarwinArtBionicStdioFunction)darwin_art_bionic_setbuf},
 {"stderr",(DarwinArtBionicStdioFunction)&darwin_art_bionic_stderr},
 {"stdin",(DarwinArtBionicStdioFunction)&darwin_art_bionic_stdin},
 {"stdout",(DarwinArtBionicStdioFunction)&darwin_art_bionic_stdout},
 {"ungetc",(DarwinArtBionicStdioFunction)darwin_art_bionic_ungetc},
 {"vprintf",(DarwinArtBionicStdioFunction)darwin_art_bionic_vprintf},
};
DarwinArtBionicStdioFunction darwin_art_bionic_stdio_resolve(const char* n) { if(!n)return NULL; size_t l=0,h=sizeof(kBindings)/sizeof(kBindings[0]); while(l<h){size_t m=l+(h-l)/2;int o=Compare(n,kBindings[m].name);if(!o)return kBindings[m].address;if(o<0)h=m;else l=m+1;}return NULL; }
