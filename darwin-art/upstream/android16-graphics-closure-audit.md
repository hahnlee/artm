# Android 16 GraphicsJNI/HWUI strict Darwin closure

This audit composes the existing Android 16 Darwin arm64 archives in their
static-link ownership order. It neither builds a replacement symbol nor
accepts an unresolved symbol merely because the count is below a threshold.

The force-loaded roots are the Layoutlib registrar, all 61 GraphicsJNI host
members, all 81 HWUI core/host members, the five common graphics APEX members,
the framework/Freetype Skia archive required by HWUI's `whole_static_libs`, and
the two-member Android ICU initialization archive. All other archives are
normal static providers in Android.bp dependency order: skcms, androidfw,
hostgraphics, the six codec modules, Minikin, HarfBuzz, FreeType, ui-types,
nativehelper, utils/cutils/log/base, ziparchive-for-incfs, png/zlib, and ICU.

Run:

```sh
tools/audit-android16-graphics-closure.sh
```

The gate produces a force-loaded relocatable Mach-O object and three strict
classifications:

1. Any unresolved symbol that is defined by an archive already present in the
   composition is a provider ordering/extraction error and fails immediately.
2. Symbols resolved only by the final Darwin executable providers are recorded
   separately. The executable link supplies CoreFoundation, CoreGraphics,
   ImageIO, Foundation, AppKit, libc++, libSystem, lz4, and zlib. The pinned
   module-complete `libandroid-base` archive supplies its Android.bp
   `whole_static_libs` fmt member; host fmt is not an executable provider.
3. Anything still rejected by the executable linker is a missing module. The
   exact symbol set is locked; an unknown or partially changed set is a hard
   regression, not a newly tolerated unresolved list.

## Current closure result

The relocatable composition is internally ordered: none of its 415 imports is
also present in the 42,082-definition provider set. Darwin frameworks,
libc++, libSystem, lz4, and zlib resolve all 415 external imports. The final
arm64 executable links and launches successfully, so the missing-module count
is zero.

The 523-member Skia archive includes the two Android.bp/GN `android_utils`
translation units that previously owned six missing BitmapRegionDecoder and
FrontBufferedStream definitions. The separate 19-member module-complete
libbase archive includes its Android.bp whole-static fmt provider. The final
link therefore does not fall back to a Homebrew fmt dylib.

The executable dependency audit rejects CoreText and host ICU, FreeType, or
png dylib regressions. Any future relocatable import already defined by one of
the 32 supplied archives is a hard provider-order failure; any future final
linker import is emitted as an unclassified missing-module set and fails.
