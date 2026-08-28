/*
 * The guest-libdl audit validates namespace lifecycle and never invokes
 * Android platform symbols.  Keep the namespace's platform-provider boundary
 * explicit without linking the full graphics/runtime closure into this unit.
 */
extern "C" void ASharedMemory_create() {}
extern "C" void ASharedMemory_setProt() {}
extern "C" void darwin_art_android_ANativeWindow_fromSurface() {}
extern "C" void darwin_art_android_ANativeWindow_lock() {}
extern "C" void darwin_art_android_ANativeWindow_release() {}
extern "C" void darwin_art_android_ANativeWindow_setBuffersGeometry() {}
extern "C" void darwin_art_android_ANativeWindow_unlockAndPost() {}
extern "C" void darwin_art_android_platform_symbol() {}
extern "C" void darwin_art_angle_dso_symbol() {}
