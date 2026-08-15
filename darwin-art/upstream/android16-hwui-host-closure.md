# Android 16 HWUI Canvas host closure

This gate deliberately imports the Android 16 upstream implementation instead
of replacing Android graphics natives with Darwin stubs. Source identity is in
`android16-hwui.lock`.

## Required upstream translation units

The public gate is the complete registration table in
`jni/android_graphics_Canvas.cpp`. Because that table takes the address of every
Canvas and BaseCanvas native, dead stripping cannot reduce it to only the
methods exercised by the first probe.

Direct gate files:

- `jni/android_graphics_Canvas.cpp`
- `jni/Paint.cpp`
- `SkiaCanvas.cpp`
- `hwui/Canvas.cpp`

The JNI files must be built using the `android_graphics_jni` defaults from the
same `Android.bp`. The native Canvas implementation must come from
`libhwui_static`/`libhwui_defaults`, not a local compatibility class.

Important source groups pulled in by that module-level contract include:

- graphics JNI support: `Graphics.cpp`, `Utils.cpp`, `Bitmap.cpp`,
  `FontUtils.cpp`, Matrix, Path, Region, Shader, ColorFilter, Typeface, Mesh,
  NinePatch, Picture, and their JNI registration translation units;
- HWUI software implementation: `SkiaCanvas.cpp`, `hwui/Canvas.cpp`,
  `hwui/PaintImpl.cpp`, `hwui/Bitmap.cpp`, `hwui/Typeface.cpp`,
  `hwui/MinikinSkia.cpp`, `hwui/MinikinUtils.cpp`, vector drawable and effect
  helpers referenced by `SkiaCanvas.cpp`;
- the remaining `libhwui_defaults` source list because the upstream module
  couples RenderNode, recording Canvas, animation, and pipeline symbols even
  when the first surface is CPU rasterized.

## Project/module dependencies

The Android 16 host definitions name these non-system modules:

- `libskia`
- `libandroidfw`
- `libminikin`
- `libharfbuzz_ng`
- `libft2`
- `libicuuc` and `libicui18n`
- `libbase`, `liblog`, `libutils`, `libcutils`, and `libui-types`
- `libnativehelper_jvm` headers/helpers
- image/codec modules named by `android_graphics_jni` when its full table is
  linked; these can only be omitted by an upstream build option, not by a
  locally rewritten registration table.

The AOSP Darwin host variant of `libhwui_defaults` defines `HWUI_NULL_GPU`.
That is suitable for compiling the complete software Canvas/RenderNode closure,
but it is not the final Metal renderer. Metal presentation remains owned by the
persistent IOSurface bridge until a Darwin HWUI pipeline exists.

## Darwin compile findings

The four direct Android 16 translation units compile unmodified as arm64 Mach-O
objects with Apple's Clang. The reproducible command is
`tools/compile-android16-hwui-canvas-gate.sh`; it intentionally performs an
object compile rather than pretending that the full registration table is
already link-closed.

At this direct-object gate there are 284 unique undefined symbols after object
headings are excluded (the generated authoritative list is
`_build/hwui-canvas-gate/undefined-symbols.txt`). That list includes platform
C/C++ runtime symbols as well as the expected Skia, GraphicsJNI, libhwui,
Minikin, liblog, and JNI helper closure. The next gate must resolve it by adding
the upstream modules above; selectively implementing unresolved names locally
would recreate the stubs this port is intended to remove.

Both Skia and HWUI must be compiled with `SK_BUILD_FOR_ANDROID_FRAMEWORK`.
Without the same definition on both sides, Android-private Skia API is absent:

- `Sk3DView::setCameraLocation()` and `getCameraLocationZ()` are hidden;
- `SkAndroidFrameworkUtils` is not declared or compiled;
- Android's extra Canvas virtuals differ, leaving `RecordingCanvas` abstract.

The current `_build/skia/libskia.a` was generated without that definition and
therefore cannot be used to link this gate. Rebuild it with
`-DSK_BUILD_FOR_ANDROID_FRAMEWORK` and include Skia's Android framework source
closure (including `src/android/SkAndroidFrameworkUtils.cpp`) before attempting
the full link. This is an ABI/build-configuration requirement, not a symbol to
replace with a Darwin stub.

The compile gate also avoids adding `compat/` as a normal angle-bracket include
directory. Doing so intercepts `<unistd.h>` with the Darwin compatibility
overlay and conflicts with AOSP `utils/Compat.h`'s `lseek64`. It uses
`-Icompat/include` for the narrow public compatibility headers and
`-iquote compat` only for `skia_darwin_config.h`.

## Ordered compile gates

1. Materialize and checksum the exact `libs/hwui` subtree.
2. Compile the four direct translation units without modifying their JNI tables.
3. Add upstream dependency projects at the locked revisions as missing headers
   and symbols are encountered.
4. Link every native referenced by `register_android_graphics_Canvas()` and
   `register_android_graphics_Paint()`.
5. Register those upstream functions in ART and delete `ProbeCanvas` and the
   Darwin Paint implementation only after the complete registration/link audit
   passes.
