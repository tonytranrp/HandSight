LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := handsight_camera
LOCAL_SRC_FILES := jni/HandSightCamera.cpp
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/.. \
    $(LOCAL_PATH)/../../shared \
    $(NDK_ROOT)/sources/android/native_app_glue
LOCAL_CPPFLAGS := -std=c++20 -fexceptions -frtti
LOCAL_STATIC_LIBRARIES := android_native_app_glue
LOCAL_LDLIBS := -landroid -llog -lcamera2ndk -lmediandk

include $(BUILD_SHARED_LIBRARY)

$(call import-module,android/native_app_glue)
