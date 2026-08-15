#pragma once

#include "jit/jit.h"

// Normally generated/exported by libart-compiler. The interpreter-only Darwin
// bootstrap needs the ABI declaration to compile libart-runtime, but does not
// instantiate a compiler.
extern "C" art::jit::JitCompilerInterface* jit_create();
