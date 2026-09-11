/*
 * Bink decoder - standalone C port from FFmpeg
 * Copyright (c) 2009 Konstantin Shishkov, 2011 Peter Ross, etc. (FFmpeg authors)
 *
 * This file is part of the standalone Bink decoder project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file bink_common.h
 *
 * 通用宏与辅助函数，用于替代 FFmpeg 的 libavutil/libavcodec
 * 工具函数（av_malloc/av_free/av_clip/AV_RL32/av_log ...）。
 *
 * 这是唯一允许直接依赖标准 C 库的头文件。所有其他头文件都包含本文件。
 */

#ifndef BINK_COMMON_H
#define BINK_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

/* math.h 中的常量并非标准 C99 定义；此处提供可移植的替代值 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 内存管理（替代 av_malloc / av_free / av_calloc / ...）               */
/* ------------------------------------------------------------------ */

#define bink_malloc   malloc
#define bink_calloc   calloc
#define bink_realloc  realloc
#define bink_free     free

/** 释放指针并将其设置为 NULL（替代 av_freep）。 */
#define bink_freep(ptr) \
    do {                \
        if (*(ptr)) {   \
            bink_free(*(ptr)); \
            *(ptr) = NULL;     \
        }               \
    } while (0)

/* ------------------------------------------------------------------ */
/* 对齐（替代 LOCAL_ALIGNED_32 / DECLARE_ALIGNED）                      */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
#  define BINK_ALIGNED_32 __attribute__((aligned(32)))
#  define BINK_ALIGNED_16 __attribute__((aligned(16)))
#elif defined(_MSC_VER)
#  define BINK_ALIGNED_32 __declspec(align(32))
#  define BINK_ALIGNED_16 __declspec(align(16))
#else
#  define BINK_ALIGNED_32
#  define BINK_ALIGNED_16
#endif

/* ------------------------------------------------------------------ */
/* 整数运算（替代 FFMIN/FFMAX/FFALIGN/av_clip/av_log2）                 */
/* ------------------------------------------------------------------ */

#define bink_min(a, b) ((a) > (b) ? (b) : (a))
#define bink_max(a, b) ((a) > (b) ? (a) : (b))
#define bink_ffmin    bink_min
#define bink_ffmax    bink_max
#define FFMIN(a, b)   bink_min(a, b)
#define FFMAX(a, b)   bink_max(a, b)

#define FFALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define FFALIGN32(x)  (((x) + 31) & ~31)

static inline int bink_clip(int a, int amin, int amax)
{
    if      (a < amin) return amin;
    else if (a > amax) return amax;
    else               return a;
}
#define av_clip bink_clip
#define bink_clip_int8(a)    bink_clip((a), -128, 127)
#define bink_clip_uint8(a)   bink_clip((a), 0, 255)

/* 2 的幂值的整数 log2，对 0 未定义 */
static inline int bink_log2(unsigned int v)
{
    int n = 0;
    while (v >>= 1)
        n++;
    return n;
}
#define av_log2 bink_log2

/* ------------------------------------------------------------------ */
/* 字节序辅助函数（替代 AV_RL32 / AV_RL16 / AV_RB32）                   */
/* ------------------------------------------------------------------ */

static inline uint32_t bink_rl32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0])       |
           ((uint32_t)b[1] <<  8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}
#define AV_RL32(p) bink_rl32(p)

static inline uint16_t bink_rl16(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}
#define AV_RL16(p) bink_rl16(p)

static inline uint32_t bink_rb32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) |
           ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] <<  8) |
           ((uint32_t)b[3]);
}
#define AV_RB32(p) bink_rb32(p)

#define AV_WL32(p, val) do {                                  \
        uint8_t *d = (uint8_t *)(p);                          \
        uint32_t v = (uint32_t)(val);                         \
        d[0] = (uint8_t)(v);      d[1] = (uint8_t)(v >> 8);   \
        d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24); \
    } while (0)

#define MKTAG(a, b, c, d) ((a) | ((b) << 8) | ((c) << 16) | ((d) << 24))
#define AV_RL32_SIG(c1, c2, c3, c4) \
    (MKTAG(c1, c2, c3, c4) & 0xFFFFFF)

/* ------------------------------------------------------------------ */
/* 日志（替代 av_log）                                                  */
/* ------------------------------------------------------------------ */

#define BINK_LOG_ERROR   0
#define BINK_LOG_WARNING 1
#define BINK_LOG_INFO    2
#define BINK_LOG_DEBUG   3

#define AV_LOG_ERROR     BINK_LOG_ERROR
#define AV_LOG_WARNING   BINK_LOG_WARNING
#define AV_LOG_INFO      BINK_LOG_INFO

#ifndef BINK_LOG_LEVEL
#define BINK_LOG_LEVEL BINK_LOG_WARNING
#endif

/* ioEF: stderr is invisible in the Windows GUI build and on Android, so the host
   installs a sink that forwards to Com_Printf. NULL keeps upstream behaviour. */
extern void (*bink_log_hook)(const char *msg);

static inline void bink_log(int level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static inline void bink_log(int level, const char *fmt, ...)
{
    va_list ap;
    if (level > BINK_LOG_LEVEL)
        return;
    va_start(ap, fmt);
    if (bink_log_hook) {
        char msg[1024];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        bink_log_hook(msg);
    } else {
        vfprintf(stderr, fmt, ap);
    }
    va_end(ap);
}

#define av_log(ctx, level, ...) bink_log((level), __VA_ARGS__)
#define bink_dlog(ctx, ...)     bink_log(BINK_LOG_DEBUG, __VA_ARGS__)
#define ff_dlog(ctx, ...)       bink_log(BINK_LOG_DEBUG, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* 错误码（替代 AVERROR_*）                                             */
/* ------------------------------------------------------------------ */

#define BINK_ERROR_INVALIDDATA (-1094995529)  /* 对应 AVERROR_INVALIDDATA */
#define BINK_ERROR_ENOMEM      (-12)          /* 对应 AVERROR(ENOMEM)     */
#define BINK_ERROR_EINVAL      (-22)          /* 对应 AVERROR(EINVAL)     */
#define BINK_ERROR_EOF         (-541478725)   /* 对应 AVERROR_EOF         */
#define BINK_ERROR_EIO         (-5)           /* 对应 AVERROR(EIO)        */
#define BINK_ERROR_NOTSUPPORTED (-95)         /* 对应 AVERROR(ENOTSUP)    */

#define AVERROR_INVALIDDATA BINK_ERROR_INVALIDDATA
#define AVERROR_ENOMEM      BINK_ERROR_ENOMEM
#define AVERROR_EINVAL      BINK_ERROR_EINVAL
#define AVERROR_EOF         BINK_ERROR_EOF
#define AVERROR_EIO         BINK_ERROR_EIO
#define AVERROR_NOTSUPPORTED BINK_ERROR_NOTSUPPORTED

/* ------------------------------------------------------------------ */
/* 其他                                                                */
/* ------------------------------------------------------------------ */

/** 在整数/浮点混合代码之后刷新 x87 FPU 栈（仅限 x86）。 */
static inline void bink_emms(void)
{
    /* C 实现不使用 MMX，无需任何操作 */
}
#define emms_c() bink_emms()

/** 附加在每个数据包之后的缓冲区填充（替代
 *  AV_INPUT_BUFFER_PADDING_SIZE），以便位读取器可以安全地
 *  越界多读这么多字节。 */
#define BINK_INPUT_BUFFER_PADDING 64

/** 受限指针提示（替代 av_restrict）。 */
#if defined(__GNUC__) || defined(__clang__)
#define bink_restrict __restrict__
#else
#define bink_restrict restrict
#endif
#define av_restrict bink_restrict

#ifdef __cplusplus
}
#endif

#endif /* BINK_COMMON_H */
