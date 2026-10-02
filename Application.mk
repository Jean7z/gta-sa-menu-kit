APP_STL := c++_static
# arm64-v8a only. SA 2.10 ships arm64 exclusively and is the only libGTASA.so the
# hook offsets here were verified against; SA 2.00 has arm7 but those offsets
# were never checked against its binary, so an armeabi-v7a build would be a
# guess that crashes at runtime.
APP_ABI := arm64-v8a
APP_PLATFORM := android-24