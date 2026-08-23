#include "darwin_art_bionic_math.h"

#include <cstring>
#include <math.h>

namespace {

void DarwinSincosf(float value, float *sine, float *cosine) {
  __sincosf(value, sine, cosine);
}

template <typename T>
void *Address(T function) {
  return reinterpret_cast<void *>(function);
}

}  // namespace

extern "C" void* darwin_art_bionic_math_resolve(const char* soname,
                                                 const char* symbol,
                                                 const char* version) {
  if (soname == nullptr || symbol == nullptr || version == nullptr ||
      std::strcmp(soname, "libm.so") != 0 || std::strcmp(version, "LIBC") != 0)
    return nullptr;
#define D(name) \
  if (std::strcmp(symbol, #name) == 0) \
    return Address(static_cast<double (*)(double)>(&name))
#define F(name) \
  if (std::strcmp(symbol, #name) == 0) \
    return Address(static_cast<float (*)(float)>(&name))
#define DD(name) \
  if (std::strcmp(symbol, #name) == 0) \
    return Address(static_cast<double (*)(double, double)>(&name))
#define FF(name) \
  if (std::strcmp(symbol, #name) == 0) \
    return Address(static_cast<float (*)(float, float)>(&name))
  D(acos); F(acosf); D(acosh); D(asin); F(asinf); D(asinh);
  D(atan); DD(atan2); FF(atan2f); F(atanf); D(atanh);
  D(cbrt); F(cbrtf); D(cos); F(cosf); D(cosh); F(erff);
  D(exp); D(exp2); F(exp2f); F(expf); DD(fmod); FF(fmodf);
  if (std::strcmp(symbol, "ilogbf") == 0)
    return Address(static_cast<int (*)(float)>(&ilogbf));
  D(log); D(log2); F(log2f); FF(nextafterf);
  DD(pow); FF(powf); DD(remainder); D(sin);
  if (std::strcmp(symbol, "sincosf") == 0) return Address(&DarwinSincosf);
  F(sinf); D(sinh); D(tan); F(tanf); D(tanh);
#undef D
#undef F
#undef DD
#undef FF
  return nullptr;
}
