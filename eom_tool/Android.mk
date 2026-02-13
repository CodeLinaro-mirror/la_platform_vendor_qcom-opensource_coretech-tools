LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := eom_tool
LOCAL_MODULE_TAGS := optional
LOCAL_VENDOR_MODULE := true
LOCAL_SRC_FILES := eom_tool.c
# Instead of TARGET_OUT_HEADERS or KERNEL_OBJ, use header library
LOCAL_HEADER_LIBRARIES := qti_kernel_headers
# Moved to data partition for consistent availability
LOCAL_MODULE_PATH := $(TARGET_OUT_DATA)/coretech-tools

#Compiler flags
LOCAL_CFLAGS := -Wall -Wextra -Werror
#Dynamic libraries

LOCAL_SHARED_LIBRARIES := libc libm
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE_TAGS := optional
include $(BUILD_EXECUTABLE)
