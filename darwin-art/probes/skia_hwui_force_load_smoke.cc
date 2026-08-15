#include <cstdint>
#include <cstdio>

#include "include/core/SkFontMgr.h"
#include "include/ports/SkFontMgr_empty.h"
#include "tools/SkSharingProc.h"

int main() {
    sk_sp<SkFontMgr> font_manager = SkFontMgr_New_Custom_Empty();
    SkSharingSerialContext sharing_context{};
    sharing_context.setDirectContext(nullptr);

    const auto serialize = &SkSharingSerialContext::serializeImage;
    const bool ok = font_manager && font_manager->countFamilies() == 1 && serialize != nullptr;
    std::printf("Skia HWUI force-load: emptyFamilies=%d sharing=%s\n",
                font_manager ? font_manager->countFamilies() : -1, ok ? "yes" : "no");
    return ok ? 0 : 1;
}
