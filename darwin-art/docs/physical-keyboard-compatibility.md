# Physical keyboard compatibility

## Status and decision

Darwin ART exposes the Mac keyboard to applications as an external Android
`InputDevice` with `SOURCE_KEYBOARD` and `KeyCharacterMap.FULL`. AppKit key
events enter the same `InputChannel`/`InputEventReceiver` path used by Android
window input. Device id `-1` remains the Android virtual-keyboard fallback; it
does not represent the host keyboard.

Input event timestamps are sampled from `CLOCK_MONOTONIC`, matching Android's
InputReader clock and Chromium's `base::TimeTicks`. `NSEvent.timestamp` is not
forwarded directly: on macOS it can differ from that clock by accumulated
system-sleep time, which makes a current event appear older than a renderer's
document time origin.

This establishes the correct framework event route, but character mapping is
not complete. `KeyMapGetCharacter` in
`compat/darwin_framework_binder_natives.cc` currently contains a small,
hard-coded US-QWERTY table for letters, digits, and common punctuation. The
native map object stores only a device id, and display-label, event synthesis,
fallback-action, and overlay operations are absent or conservative stubs.

The table was introduced as a bootstrap compatibility bridge so unmodified
AOSP applications could accept physical-keyboard input while the common event
pipeline was brought up. It is not the target architecture and must not grow
into a second keyboard-layout implementation one switch case at a time.

## Android contract

Android separates physical keys from produced text:

1. A device-specific key layout (`.kl`) maps Linux scan codes or HID usages to
   Android key codes.
2. A device-specific key character map (`.kcm`) maps an Android key code plus a
   normalized meta state to characters, dead keys, fallback actions, display
   labels, and numeric labels.
3. `KeyEvent` preserves the physical event identity: device id, scan code, key
   code, source, meta state, repeat count, and timestamps.
4. `KeyCharacterMap.nativeGetCharacter()` crosses JNI and delegates to the
   native per-device `KeyCharacterMap`; Java applies Android's combining-accent
   conventions where required.

This is distinct from IME composition. An IME edits text through
`InputMethodManager` and `InputConnection`; it must not be simulated by
returning composed strings from `KeyMapGetCharacter`.

Authoritative AOSP implementations:

- [Framework JNI bridge](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/jni/android_view_KeyCharacterMap.cpp)
- [Native KeyCharacterMap](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/libs/input/KeyCharacterMap.cpp)
- [Java KeyCharacterMap contract](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/view/KeyCharacterMap.java)
- [InputReader/EventHub device path](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/services/inputflinger/reader/EventHub.cpp)

## Observable compatibility gaps

Applications can currently distinguish Darwin ART from Android in these ways:

- The active macOS layout and actual keyboard device do not affect the fixed
  US-QWERTY character table.
- Caps Lock, Option/Alt, AltGr, Control combinations, dead keys, compose
  sequences, and non-US layouts are incomplete.
- ANSI, ISO, and JIS physical key positions are not represented by distinct
  device layouts.
- `getDisplayLabel`, `getNumber`, `getMatch`, `getFallbackAction`, and
  `getEvents` do not yet form a coherent per-device map.
- Keyboard hotplug and host input-source changes do not publish a new Android
  input-device generation or invalidate a cached map.
- Editable-text composition and marked text do not yet have a complete
  `InputConnection` path.

## Target ownership

```text
NSEvent and IOHID device identity
              |
              v
Darwin input device registry (Rust)
  - stable Android device ids and generations
  - immutable physical layout (.kl-equivalent)
  - immutable character map (.kcm-equivalent)
              |
              v
versioned, table-oriented C ABI
              |
              v
Android InputDevice / KeyCharacterMap / KeyEvent
              |
              v
InputChannel -> ViewRootImpl -> application
```

Rust should own device identity, validated map data, cache lifetime, layout
generations, and hotplug transitions. AppKit/Carbon adapters may obtain the
active macOS input source and use `UCKeyTranslate` to compile a deterministic
host-layout overlay. JNI C++ remains a narrow ABI adapter over immutable map
queries; it must not own a parallel mutable keyboard model.

The runtime should parse Android `.kl` and `.kcm` data so Android's generic and
device-specific maps remain the semantic baseline. A host-generated overlay
may provide characters for the selected macOS layout, but it must preserve
Android key codes, meta-state normalization, dead-key representation, and
fallback behavior. Host translation should occur when a map generation is
built, not as an untracked process-global lookup during an arbitrary JNI call.

## Delivery milestones

1. Replace the C++ switch with an immutable Rust-owned map and table lookup,
   while preserving current US-QWERTY behavior.
2. Parse Android `.kl` and `.kcm` files and select generic/device-specific maps
   using stable input-device descriptors.
3. Compile macOS TIS/`UCKeyTranslate` layouts into overlays; cover ANSI, ISO,
   JIS, Shift, Caps Lock, Option/Alt, AltGr-equivalent, and dead keys.
4. Publish device hotplug and layout changes with Android-style generations
   and cache invalidation.
5. Implement marked-text and composition separately through the Android
   `InputMethodManager`/`InputConnection` boundary.

## Completion criteria

This work is complete only when all of the following hold:

- `KeyMapGetCharacter` contains no hard-coded US-layout switch.
- Physical keyboard events remain external `FULL` keyboard events; the
  virtual device id is used only as Android's fallback map.
- Character, display-label, number, match, fallback-action, and event-synthesis
  queries agree with the selected Android character map.
- Shift, Caps Lock, Option/Alt, dead-key composition, and at least one non-US
  layout pass differential tests against AOSP behavior.
- ANSI, ISO, and JIS devices have verified scan-code/key-code position tests.
- A host layout change is reflected without restarting the application, via a
  defined input-device generation change.
- Unmodified AOSP Calculator, Calendar, DeskClock, and an editable `TextView`
  pass through the production input path without fixture-specific shortcuts.
- IME composition tests pass through `InputConnection` rather than extending
  the physical-key character-map bridge.
- Pointer and key event times stay in Android's monotonic clock domain across
  host sleep/wake and never regress relative to renderer/document time.
