# Android APK app runtime gate

This gate builds and inspects a real Android APK whose launcher Activity is
declared in binary `AndroidManifest.xml`, whose code lives in the APK's single
`classes.dex`, and which contains no native `.so` entry.

The bounded inspector rejects ZIP64, encryption, duplicate or unsafe paths,
secondary DEX files, malformed binary XML, ambiguous launchers, and any native
library. The fixture programmatically creates real `TextView`, `CheckBox`,
`RadioButton`, `ToggleButton`, `SeekBar`, `ProgressBar`, and `Button` instances.
It loads the pinned Android 16 `framework-res.apk` and applies the framework's
built-in Material Light theme. The fixture adds only a restrained Material You
light-scheme palette and a software-compatible Material ripple drawable for the primary
button; widget state lists, typography, animation, and rendering remain owned
by Android framework code.

Run:

```sh
./tools/android-apk-app-runtime/audit.sh
```

The generated APK contains only the fixture's `MainActivity` and its
`SystemFonts` bootstrap; Darwin runtime helpers remain in a separate support
DEX that is added to the same `PathClassLoader`. The generated APK is
`_build/android-apk-app-runtime/simple-no-native.apk`. The integration target
is to pass that exact APK to ART's DEX loader, instantiate the manifest launcher
through the APK `PathClassLoader`, execute `Activity.onCreate`, attach a real
`PhoneWindow`/`DecorView`, and present its 360x640 frame in the Darwin host
window.

The acceptance resolves all seven tagged children back to their exact Android
framework widget types and requires a fully opaque, visually diverse 360x640
frame. It therefore does not accept custom `View` stand-ins, app-painted
rectangles, or a blank opaque frame. The host provides a synthetic 60 Hz display
clock and the framework's `PropertyValuesHolder`/`NativeAllocationRegistry`
hooks. ValueAnimator/Choreographer timing remains Android-driven; the patterned
framework RippleDrawable path is GPU-only on Android 16 and is therefore replaced
by the software-compatible drawable on this host Canvas.

The Android-side GPU handoff has a separate opt-in gate:

```sh
./tools/accept-android16-hwui-gpu-layoutlib.sh
```

It compiles the checksum-verified Layoutlib registrar with
`DARWIN_ART_HWUI_GPU=1`; only in that mode does Layoutlib stop forcing
`RenderPipelineType::SkiaCpu`. The normal registrar build remains CPU-safe.
The direct Metal replay gate then verifies framework `RecordingCanvas` display
list replay, a Material-style button/ripple frame, zero CPU readback, and zero
full-frame blits:

```sh
./tools/run-android16-hwui-metal-replay-gate.sh
```

After the graphics runtime has been built, run any APK in the current bounded
programmatic-UI subset with:

```sh
./tools/run-android-apk-app.sh path/to/app.apk 30
```

Omitting the duration keeps the window available for up to 24 hours and still
exits immediately when the user closes it.

The interactive runner renders a 720x1280 Android surface into a 360x640
logical macOS window with a 2x `CAMetalLayer` backing scale. Framework density
is set to 320 dpi for that mode, so widget size is unchanged while Retina
sharpness is preserved. Headless acceptance remains 360x640 for stable,
low-cost regression captures. Mouse down/up events use backing-pixel
coordinates to hit-test the retained Android `DecorView`; clickable widgets
execute `View.performClick()` and the resulting framework state is rerendered.
This is intentionally narrower than Android `MotionEvent` gesture dispatch, so
dragging the `SeekBar`, multitouch, scrolling, and long-press remain unsupported.

The runner never extracts or rewrites the APK. It rejects APKs containing
native libraries, secondary DEX files, or an ambiguous launcher before ART is
created. App resource-table/layout inflation and Android system services are
not yet general; the first supported APK class builds its view hierarchy in
Java from framework widgets or custom views. `Switch` still requires the
unintegrated VelocityTracker native table, and editable text requires the
Editor/input-method service slice, so neither is claimed by this gate.
