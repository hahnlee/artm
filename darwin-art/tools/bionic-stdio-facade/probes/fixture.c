#include <bits/struct_file.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

extern FILE __sF[];
static FILE* gRace;
static int Equal(const unsigned char* a,const unsigned char* b,size_t n){for(size_t i=0;i<n;i++)if(a[i]!=b[i])return 0;return 1;}

__attribute__((visibility("default"))) int bionic_stdio_fixture_basic(void){
 FILE* in=&__sF[0];FILE* out=&__sF[1];FILE* err=&__sF[2];
 if(fileno(in)!=0||fileno(out)!=1||fileno(err)!=2)return 1;
 if(getc(in)!='i'||ungetc('Z',in)!='Z'||getc(in)!='Z'||getc(in)!='n')return 2;
 if(fputc('!',out)!='!'||fflush(out)!=0||fflush(NULL)!=0)return 3;
 FILE* f=fopen("/system/input.bin","rb");if(f==NULL||fileno(f)<20000)return 4;
 unsigned char b[8]={0};if(fread(b,2,2,f)!=2||!Equal(b,(const unsigned char*)"abcd",4)||ftello(f)!=4)return 5;
 if(getc(f)!='e'||ungetc('Q',f)!='Q'||getc(f)!='Q')return 6;
 if(fseek(f,0,SEEK_SET)!=0||getc(f)!='a')return 7;
 errno=0;if(fwrite("x",1,1,f)!=0||errno!=EBADF)return 8;
 if(fclose(f)!=0)return 9;
 errno=0;if(getc(f)!=EOF||errno!=EBADF)return 10;
 f=fopen("/private/output.bin","w+b");if(f==NULL)return 11;
 if(fwrite("xyz",1,3,f)!=3||ftello(f)!=3||fseeko(f,0,SEEK_SET)!=0)return 12;
 if(fread(b,1,3,f)!=3||!Equal(b,(const unsigned char*)"xyz",3))return 13;
 errno=0;if(fread(b,SIZE_MAX,2,f)!=0||errno!=EOVERFLOW)return 14;
 if(fseeko(f,16*1024*1024,SEEK_SET)!=0)return 15;
 errno=0;if(fputc('x',f)!=EOF||errno!=EFBIG)return 16;
 errno=0;if(fseek(f,0,99)!=-1||errno!=EINVAL)return 17;
 if(fclose(f)!=0)return 18;
 errno=0;if(fopen("/system/input.bin","q")!=NULL||errno!=EINVAL)return 19;
 errno=0;if(fopen("","w")!=NULL||errno!=ENOENT)return 20;
 errno=0;if(fopen("/missing","rb")!=NULL||errno!=ENOENT)return 24;
 return 42;
}
__attribute__((visibility("default"))) int bionic_stdio_fixture_race_setup(void){gRace=fopen("/race","wb");return gRace?42:20;}
__attribute__((visibility("default"))) int bionic_stdio_fixture_race_write(void){int r=fputc('r',gRace);if(r=='r')return 42;if(r==EOF&&errno==EBADF)return 42;return 21;}
__attribute__((visibility("default"))) int bionic_stdio_fixture_race_close(void){return fclose(gRace)==0?42:22;}
__attribute__((visibility("default"))) int bionic_stdio_fixture_race_after(void){errno=0;return fputc('x',gRace)==EOF&&errno==EBADF?42:23;}
