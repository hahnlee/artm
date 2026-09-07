// Extend upstream's generator so assembly and C++ use one reference base.
#include "asm_defines.cc"
ASM_DEFINE(DARWIN_ART_REFERENCE_BASE, art::kArtCompressedReferenceBase)
