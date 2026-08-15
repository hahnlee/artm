#include <algorithm>
#include <string>
#include <vector>

namespace {

constexpr char kText[] =
    "darwin-art-libcxx-acceptance-crosses-the-small-string-boundary";

__attribute__((noinline)) const char* TextSource() {
  __asm__ __volatile__("" ::: "memory");
  return kText;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int
darwin_art_libcxx_collections() {
  std::string text(TextSource());
  std::vector<int> values{9, 1, 7, 3, 5};
  std::sort(values.begin(), values.end());
  return static_cast<int>(text.size()) +
         static_cast<unsigned char>(text.at(11)) + values.front() * 10 +
         values.back();
}
