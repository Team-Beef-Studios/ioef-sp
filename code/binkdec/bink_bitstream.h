/*
 * Bink decoder - standalone C port from FFmpeg
 *
 * This file is part of the standalone Bink decoder project (LGPL 2.1+).
 * See bink_common.h for the full license header.
 */

/**
 * @file bink_bitstream.h
 *
 * 独立的位流读取器（LSB 优先 / 小端位序，
 * 与使用 BITSTREAM_READER_LE 构建的 FFmpeg GetBitContext 一致，
 * 这正是 Bink 视频和音频解码器所使用的）以及 VLC 表构建器。
 * 本文件替代 FFmpeg 的 GetBitContext + get_bits.h + vlc.c。
 */

#ifndef BINK_BITSTREAM_H
#define BINK_BITSTREAM_H

#include "bink_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 位流读取器                                                          */
/* ------------------------------------------------------------------ */

typedef struct BitContext {
    const uint8_t *buffer;    /*!< 位缓冲区的起始位置                    */
    const uint8_t *buffer_end;/*!< 位缓冲区的结束位置                    */
    int index;                /*!< 当前位位置                           */
    int size_in_bits;         /*!< 有效位的总数量                        */
} BitContext;

/**
 * 在内存缓冲区上初始化位读取器。
 * @param gb    要初始化的读取器
 * @param buf   输入数据（在 size_in_bits/8 之后必须具有
 *              BINK_INPUT_BUFFER_PADDING 字节的可读余量）
 * @param bit_size 缓冲区中有效位的数量
 */
static inline void bink_init_getbits(BitContext *gb, const uint8_t *buf,
                                     int bit_size)
{
    gb->buffer       = buf;
    gb->buffer_end   = buf + (bit_size + 7) / 8;
    gb->index        = 0;
    gb->size_in_bits = bit_size;
}

/** 从字节缓冲区初始化（大小以字节为单位）。 */
static inline void bink_init_getbits8(BitContext *gb, const uint8_t *buf,
                                      int size)
{
    bink_init_getbits(gb, buf, size << 3);
}

/** 当前位位置（替代 get_bits_count）。 */
static inline int bink_get_bits_count(const BitContext *gb)
{
    return gb->index;
}
#define get_bits_count(gb) bink_get_bits_count(gb)

/** 剩余位的数量（替代 get_bits_left）。 */
static inline int bink_get_bits_left(const BitContext *gb)
{
    return gb->size_in_bits - gb->index;
}
#define get_bits_left(gb) bink_get_bits_left(gb)

/**
 * 显示接下来的 n 位而不消费它们。
 * LSB 优先位序：待读取的下一位是当前字节的第 0 位（LSB）。
 * 这与使用 BITSTREAM_READER_LE 编译的 FFmpeg GetBitContext 一致，
 * 这正是 Bink 视频和音频解码器所使用的。
 */
static inline uint32_t bink_show_bits(const BitContext *gb, int n)
{
    uint32_t cache = bink_rl32(gb->buffer + (gb->index >> 3)) >>
                     (gb->index & 7);
    return (cache << (32 - n)) >> (32 - n);
}
#define show_bits(gb, n) bink_show_bits(gb, n)

/** 读取 n（1..25）位。 */
static inline uint32_t bink_get_bits(BitContext *gb, int n)
{
    uint32_t ret = bink_show_bits(gb, n);
    gb->index += n;
    return ret;
}
#define get_bits(gb, n) bink_get_bits(gb, n)

/** 读取 1 位。 */
static inline uint32_t bink_get_bits1(BitContext *gb)
{
    uint32_t ret = (gb->buffer[gb->index >> 3] >> (gb->index & 7)) & 1;
    gb->index++;
    return ret;
}
#define get_bits1(gb) bink_get_bits1(gb)

/** 读取 n（0..32）位。 */
static inline uint32_t bink_get_bits_long(BitContext *gb, int n)
{
    if (n <= 0)
        return 0;
    if (n <= 25)
        return bink_get_bits(gb, n);
    {
        uint32_t ret = bink_get_bits(gb, 16);
        return ret | (bink_get_bits(gb, n - 16) << 16);
    }
}
#define get_bits_long(gb, n) bink_get_bits_long(gb, n)

/** 读取 0..25 位。 */
static inline int bink_get_bitsz(BitContext *gb, int n)
{
    return n ? (int)bink_get_bits(gb, n) : 0;
}
#define get_bitsz(gb, n) bink_get_bitsz(gb, n)

/** 跳过 n 位。 */
static inline void bink_skip_bits(BitContext *gb, int n)
{
    gb->index += n;
}
#define skip_bits(gb, n) bink_skip_bits(gb, n)

/** 跳过 n 位（长版本）。 */
static inline void bink_skip_bits_long(BitContext *gb, int n)
{
    gb->index += n;
}
#define skip_bits_long(gb, n) bink_skip_bits_long(gb, n)

/** 跳到下一个 32 位边界。 */
static inline void bink_get_bits_align32(BitContext *gb)
{
    int n = (-gb->index) & 31;
    if (n)
        bink_skip_bits(gb, n);
}
#define get_bits_align32(gb) bink_get_bits_align32(gb)

/* ------------------------------------------------------------------ */
/* VLC 表                                                              */
/* ------------------------------------------------------------------ */

#define BINK_VLC_TYPE int16_t

typedef struct BinkVLC {
    int bits;
    BINK_VLC_TYPE (*table)[2];   /*!< 码, 位数 */
    int table_size, table_allocated;
} BinkVLC;

typedef BinkVLC VLC;
#define VLC_TYPE BINK_VLC_TYPE

#define INIT_VLC_INPUT_LE        2
#define INIT_VLC_OUTPUT_LE       8
#define INIT_VLC_LE              (INIT_VLC_INPUT_LE | INIT_VLC_OUTPUT_LE)
#define INIT_VLC_USE_NEW_STATIC  4

/**
 * 构建适用于 bink_get_vlc2() 的 VLC 解码表。
 *
 * 算法参见 FFmpeg 的 ff_init_vlc_sparse()；本函数是它的
 * 忠实、无依赖移植。
 *
 * @param vlc      要初始化的 VLC（静态表必须预先设置
 *                 vlc->table 和 vlc->table_allocated，并传入
 *                 INIT_VLC_USE_NEW_STATIC）
 * @param nb_bits  直接查找表使用的位数
 * @param nb_codes （长度，码）条目的数量
 * @param bits     码长数组（uint8_t）
 * @param bits_wrap 条目之间的字节步长
 * @param codes    码值数组
 * @param codes_wrap 条目之间的字节步长
 * @param flags    INIT_VLC_* 的组合
 * @return 成功返回 0，否则返回负错误码
 */
int bink_init_vlc(BinkVLC *vlc, int nb_bits, int nb_codes,
                  const void *bits, int bits_wrap, int bits_size,
                  const void *codes, int codes_wrap, int codes_size,
                  const void *symbols, int symbols_wrap, int symbols_size,
                  int flags);

#define init_vlc(vlc, nb_bits, nb_codes, bits, bits_wrap, bits_size,   \
                 codes, codes_wrap, codes_size, flags)                 \
    bink_init_vlc((vlc), (nb_bits), (nb_codes),                        \
                  (bits), (bits_wrap), (bits_size),                    \
                  (codes), (codes_wrap), (codes_size),                 \
                  NULL, 0, 0, (flags))

/** 释放动态分配的 VLC 表。 */
void bink_free_vlc(BinkVLC *vlc);
#define ff_free_vlc(vlc) bink_free_vlc(vlc)

/**
 * 解码一个 VLC 码（LSB 优先位序）。
 * 等价于 FFmpeg 的 get_vlc2()（使用 BITSTREAM_READER_LE 读取器和
 * 以 INIT_VLC_LE 构建的表，如同 Bink 视频解码器所使用的）。
 *
 * @param gb    位读取器
 * @param table 使用 bink_init_vlc() + INIT_VLC_LE 构建的 VLC 表
 * @param bits  直接查找表的位数
 * @return 解码出的符号
 */
static inline int bink_get_vlc2(BitContext *gb, BINK_VLC_TYPE (*table)[2],
                                int bits)
{
    unsigned int index = bink_show_bits(gb, bits);
    int code = table[index][0];
    int n    = table[index][1];
    if (n < 0) { /* 子表 */
        bink_skip_bits(gb, bits);
        index = bink_show_bits(gb, -n) + (unsigned)code;
        code  = table[index][0];
        n     = table[index][1];
    }
    bink_skip_bits(gb, n);
    return code;
}
#define get_vlc2(gb, table, bits, max_depth) bink_get_vlc2(gb, table, bits)

#ifdef __cplusplus
}
#endif

#endif /* BINK_BITSTREAM_H */
