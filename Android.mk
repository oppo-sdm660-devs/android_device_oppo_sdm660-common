#
# Copyright (C) 2025 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

LOCAL_PATH := $(call my-dir)

ifneq ($(filter R11 R11s,$(TARGET_DEVICE)),)

include $(call all-makefiles-under,$(LOCAL_PATH))

endif
