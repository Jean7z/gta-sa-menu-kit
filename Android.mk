LOCAL_PATH := $(call my-dir)

# ---- SA Menu Kit (framework) ----
# arm64-v8a only; Application.mk pins the ABI. The module name matches AML's
# library lookup name.
include $(CLEAR_VARS)
LOCAL_MODULE := AML_PSDK_MenuKit64
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_C_INCLUDES := $(LOCAL_PATH) $(LOCAL_PATH)/psdk $(LOCAL_PATH)/mod
LOCAL_SRC_FILES := main.cpp mod/menu.cpp mod/logger.cpp mod/config.cpp
LOCAL_CXXFLAGS := -Os -ffunction-sections -fdata-sections -DNDEBUG -std=c++17
LOCAL_LDFLAGS := -Wl,--gc-sections
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)
