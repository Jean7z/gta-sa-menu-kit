LOCAL_PATH := $(call my-dir)

# Set MENUKIT_EMBEDDED_EXAMPLE=1 to build the client example with Menu Kit
# compiled into its library. The default target builds the standalone mod.
include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_C_INCLUDES := $(LOCAL_PATH) $(LOCAL_PATH)/psdk $(LOCAL_PATH)/mod
LOCAL_CXXFLAGS := -Os -ffunction-sections -fdata-sections -DNDEBUG -std=c++17
LOCAL_LDFLAGS := -Wl,--gc-sections
LOCAL_LDLIBS := -llog

ifeq ($(MENUKIT_EMBEDDED_EXAMPLE),1)
LOCAL_MODULE := MenuKitEmbeddedExample
LOCAL_CFLAGS += -DMENUKIT_EMBEDDED
LOCAL_SRC_FILES := examples/embedded/main.cpp mod/menu.cpp mod/logger.cpp
else
# The module name matches AML's library lookup name.
LOCAL_MODULE := AML_PSDK_MenuKit64
LOCAL_SRC_FILES := main.cpp mod/menu.cpp mod/logger.cpp mod/config.cpp
endif

include $(BUILD_SHARED_LIBRARY)
