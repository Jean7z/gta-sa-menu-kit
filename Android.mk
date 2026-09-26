LOCAL_PATH := $(call my-dir)

# ---- SA Menu Kit (framework) ----
include $(CLEAR_VARS)
LOCAL_MODULE := AML_PSDK_MenuKit
ifeq ($(TARGET_ARCH_ABI), arm64-v8a)
LOCAL_MODULE := AML_PSDK_MenuKit64
endif
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_C_INCLUDES := $(LOCAL_PATH) $(LOCAL_PATH)/psdk $(LOCAL_PATH)/mod
LOCAL_SRC_FILES := main.cpp mod/menu.cpp mod/logger.cpp mod/config.cpp
LOCAL_CXXFLAGS := -Os -ffunction-sections -fdata-sections -DNDEBUG -std=c++17
LOCAL_LDFLAGS := -Wl,--gc-sections
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)

# ---- Car Spawner (client demo of the MenuKit API) ----
include $(CLEAR_VARS)
LOCAL_MODULE := AML_PSDK_CarSpawner
ifeq ($(TARGET_ARCH_ABI), arm64-v8a)
LOCAL_MODULE := AML_PSDK_CarSpawner64
endif
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_C_INCLUDES := $(LOCAL_PATH) $(LOCAL_PATH)/psdk $(LOCAL_PATH)/mod
LOCAL_SRC_FILES := examples/car-spawner/main.cpp mod/logger.cpp mod/config.cpp
LOCAL_CXXFLAGS := -Os -ffunction-sections -fdata-sections -DNDEBUG -std=c++17
LOCAL_LDFLAGS := -Wl,--gc-sections
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)