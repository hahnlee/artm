# Android APK app runtime gate

This gate builds and inspects a real Android APK whose launcher Activity is
declared in binary `AndroidManifest.xml`, whose code lives in the APK's single
`classes.dex`, and which contains no native `.so` entry.

The bounded inspector rejects ZIP64, encryption, duplicate or unsafe paths,
secondary DEX files, malformed binary XML, ambiguous launchers, and any native
library. The fixture programmatically creates a real `android.widget.Button`,
so no app resource inflation is required for the first widget vertical slice.

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
`PhoneWindow`/`DecorView`, and present its 640x360 frame in the Darwin host
window.

The acceptance resolves the child view back to an actual
`android.widget.Button` instance and requires the captured frame to contain the
exact dark background, blue rounded-button, and white Roboto text colors. It
therefore does not accept a custom `View` stand-in or a blank opaque frame.

After the graphics runtime has been built, run any APK in the current bounded
programmatic-UI subset with:

```sh
./tools/run-android-apk-app.sh path/to/app.apk 30
```

The runner never extracts or rewrites the APK. It rejects APKs containing
native libraries, secondary DEX files, or an ambiguous launcher before ART is
created. App resource-table/layout inflation and Android system services are
not yet general; the first supported APK class builds its view hierarchy in
Java from framework widgets or custom views.
