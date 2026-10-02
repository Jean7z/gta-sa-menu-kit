LOCAL_PATH := $(call my-dir)

# ---- SA Menu Kit (framework) ----
# arm64-v8a only (ver Application.mk). The module keeps its plain name as a
# fallback so a stray armeabi-v7a APP_ABI fails loudly on the missing symbol
# instead of silently shipping a .so with the wrong name.
include $(CLEAR_VARS)
LOCAL_MODULE := AML_PSDK_MenuKit64
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_C_INCLUDES := $(LOCAL_PATH) $(LOCAL_PATH)/psdk $(LOCAL_PATH)/mod
LOCAL_SRC_FILES := main.cpp mod/menu.cpp mod/logger.cpp mod/config.cpp
LOCAL_CXXFLAGS := -Os -ffunction-sections -fdata-sections -DNDEBUG -std=c++17
LOCAL_LDFLAGS := -Wl,--gc-sections
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)

# ---- Internet Radio (reproductor local: decodifica con la plataforma) ----
include $(CLEAR_VARS)
LOCAL_MODULE := AML_PSDK_InternetRadio64
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_C_INCLUDES := $(LOCAL_PATH) $(LOCAL_PATH)/psdk $(LOCAL_PATH)/mod \
                   $(LOCAL_PATH)/examples/internet-radio
LOCAL_SRC_FILES := examples/internet-radio/main.cpp \
                   examples/internet-radio/radio/decode.c \
                   examples/internet-radio/radio/ring.c \
                   examples/internet-radio/radio/sink.c \
                   examples/internet-radio/radio/sink_opensl.c \
                   examples/internet-radio/radio/meddec.c \
                   examples/internet-radio/radio/playlist.c \
                   examples/internet-radio/radio/local.c \
                   examples/internet-radio/radio/cover.c \
                   mod/logger.cpp mod/config.cpp
LOCAL_CXXFLAGS := -Os -ffunction-sections -fdata-sections -DNDEBUG -std=c++17
LOCAL_LDFLAGS := -Wl,--gc-sections
# -lmediandk = AMediaExtractor/AMediaCodec. mbedtls ya no se enlaza: el
# reproductor es local, no hay nada que descargar (ver Android.mk.internetradio).
LOCAL_LDLIBS := -llog -lOpenSLES -lmediandk
include $(BUILD_SHARED_LIBRARY)