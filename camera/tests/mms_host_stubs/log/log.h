/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <stdio.h>
#define ALOGW(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
