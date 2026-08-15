#include "darwin_art_bionic_stdio.h"
#include <errno.h>
#include <stddef.h>

DarwinArtAndroidFile darwin_art_bionic___sF[3];
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
int darwin_art_bionic_getc(DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_getc_core(f)); }
int darwin_art_bionic_ungetc(int c,DarwinArtAndroidFile* f) { WRAP(e, darwin_art_bionic_stdio_ungetc_core(c,f)); }

static int Compare(const char* a,const char* b) { while(*a==*b&&*a){a++;b++;} return (unsigned char)*a<(unsigned char)*b?-1:((unsigned char)*a!=(unsigned char)*b); }
typedef struct { const char* name; DarwinArtBionicStdioFunction address; } Binding;
static const Binding kBindings[] = {
 {"__sF",(DarwinArtBionicStdioFunction)darwin_art_bionic___sF},
 {"fclose",(DarwinArtBionicStdioFunction)darwin_art_bionic_fclose},
 {"fflush",(DarwinArtBionicStdioFunction)darwin_art_bionic_fflush},
 {"fileno",(DarwinArtBionicStdioFunction)darwin_art_bionic_fileno},
 {"fopen",(DarwinArtBionicStdioFunction)darwin_art_bionic_fopen},
 {"fputc",(DarwinArtBionicStdioFunction)darwin_art_bionic_fputc},
 {"fread",(DarwinArtBionicStdioFunction)darwin_art_bionic_fread},
 {"fseek",(DarwinArtBionicStdioFunction)darwin_art_bionic_fseek},
 {"fseeko",(DarwinArtBionicStdioFunction)darwin_art_bionic_fseeko},
 {"ftello",(DarwinArtBionicStdioFunction)darwin_art_bionic_ftello},
 {"fwrite",(DarwinArtBionicStdioFunction)darwin_art_bionic_fwrite},
 {"getc",(DarwinArtBionicStdioFunction)darwin_art_bionic_getc},
 {"ungetc",(DarwinArtBionicStdioFunction)darwin_art_bionic_ungetc},
};
DarwinArtBionicStdioFunction darwin_art_bionic_stdio_resolve(const char* n) { if(!n)return NULL; size_t l=0,h=sizeof(kBindings)/sizeof(kBindings[0]); while(l<h){size_t m=l+(h-l)/2;int o=Compare(n,kBindings[m].name);if(!o)return kBindings[m].address;if(o<0)h=m;else l=m+1;}return NULL; }
