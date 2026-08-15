#include "darwin_art_bionic_allocator.h"
#include "darwin_art_bionic_errno.h"
#include "darwin_art_bionic_format.h"
#include "darwin_art_elf_loader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

DarwinArtElfResolveStatus Resolve(void*,const DarwinArtElfSymbolRequest* request,uintptr_t* address,DarwinArtElfErrorBuffer*) {
  if(!request||!address||!request->symbol)return DARWIN_ART_ELF_RESOLVE_ERROR;
  if(auto f=darwin_art_bionic_format_resolve(request->symbol)){*address=reinterpret_cast<uintptr_t>(f);return DARWIN_ART_ELF_RESOLVE_FOUND;}
  if(auto f=darwin_art_bionic_allocator_resolve(request->symbol)){*address=reinterpret_cast<uintptr_t>(f);return DARWIN_ART_ELF_RESOLVE_FOUND;}
  if(auto f=darwin_art_bionic_errno_resolve(request->symbol)){*address=reinterpret_cast<uintptr_t>(f);return DARWIN_ART_ELF_RESOLVE_FOUND;}
  return DARWIN_ART_ELF_RESOLVE_ERROR;
}

int main(int argc,char** argv) {
  if(argc!=2)return 2;std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});if(bytes.empty())return 3;
  DarwinArtElfLoadOptions options{DARWIN_ART_ELF_ABI_VERSION,&Resolve,nullptr};DarwinArtElfHandle* handle=nullptr;char message[512]{};DarwinArtElfErrorBuffer error{message,sizeof(message),0};
  if(darwin_art_elf_load_bytes(bytes.data(),bytes.size(),&options,&handle,&error)!=DARWIN_ART_ELF_OK){std::fprintf(stderr,"load: %s\n",message);return 4;}
  uintptr_t address=0;if(darwin_art_elf_lookup(handle,"format_fixture_run",&address,&error)!=DARWIN_ART_ELF_OK)return 5;
  int result=reinterpret_cast<int(*)()>(address)();
  if(darwin_art_elf_unload(&handle,&error)!=DARWIN_ART_ELF_OK)return 6;
  if(result!=42){std::fprintf(stderr,"fixture=%d\n",result);return 7;}
  std::puts("bionic-format-elf: PASS Android-AAPCS64 variadic+va_list relocation/call allocator+errno=owned");return 0;
}
