/*
 * Bink decoder - standalone C port from FFmpeg
 *
 * Bink video decoder
 * Copyright (c) 2009 Konstantin Shishkov
 * Copyright (C) 2011 Peter Ross <pross@xvid.org>
 * Bink DSP routines
 * Copyright (c) 2009 Konstantin Shishkov
 * Block DSP (fill/clear) - FFmpeg authors
 *
 * This file is part of the standalone Bink decoder project (LGPL 2.1+).
 * See bink_common.h for the full license header.
 */

/**
 * @file bink_video.c
 *
 * FFmpeg Bink 视频解码器的无依赖移植版本。
 *  - libavcodec/bink.c     (Huffman 束、块解码、运动补偿)
 *  - libavcodec/binkdsp.c  (IDCT、scale_block、add_pixels8)
 *  - libavcodec/blockdsp.c (clear_block、fill_block)
 *  - libavcodec/hpeldsp.c  (put_pixels8x8)
 */

#include "bink_video.h"
#include "bink_common.h"
#include "bink_bitstream.h"
#include "bink_data.h"

#define BINK_FLAG_ALPHA 0x00100000
#define BINK_FLAG_GRAY  0x00020000

/* ------------------------------------------------------------------ */
/* 数据结构                                                          */
/* ------------------------------------------------------------------ */

/** 解码 4 位 Huffman 编码值所需的数据 */
typedef struct Tree {
    int     vlc_num;  ///< 树编号（位于 bink_trees[] 中）
    uint8_t syms[16]; ///< 叶子值到符号的映射
} Tree;

#define GET_HUFF(gb, tree)  (tree).syms[bink_get_vlc2(gb, \
                                v->bink_trees[(tree).vlc_num].table, \
                                v->bink_trees[(tree).vlc_num].bits)]

/** 用于解码单个 Bink 数据类型的数据结构 */
typedef struct Bundle {
    int     len;       ///< 要解码的条目数量对应的长度（以位为单位）
    Tree    tree;      ///< Huffman 树相关数据
    uint8_t *data;     ///< 已解码符号的缓冲区
    uint8_t *data_end; ///< 缓冲区末尾
    uint8_t *cur_dec;  ///< 指向缓冲区中尚未解码部分的指针
    uint8_t *cur_ptr;  ///< 指向尚未从缓冲区读取的数据的指针
} Bundle;

/**
 * Bink 视频编解码器中使用的不同数据类型的 ID
 */
enum Sources {
    BINK_SRC_BLOCK_TYPES = 0, ///< 8x8 块类型
    BINK_SRC_SUB_BLOCK_TYPES, ///< 16x16 块类型（8x8 块类型的子集）
    BINK_SRC_COLORS,          ///< 用于不同块类型的像素值
    BINK_SRC_PATTERN,         ///< 用于双色图案填充的 8 位值
    BINK_SRC_X_OFF,           ///< 运动矢量的 X 分量
    BINK_SRC_Y_OFF,           ///< 运动矢量的 Y 分量
    BINK_SRC_INTRA_DC,        ///< 带 DCT 的帧内块的 DC 值
    BINK_SRC_INTER_DC,        ///< 带 DCT 的帧间块的 DC 值
    BINK_SRC_RUN,             ///< 特殊填充块的行程长度

    BINK_NB_SRC
};

/**
 * 旧版 Bink 视频编解码器中使用的不同数据类型的 ID
 */
enum OldSources {
    BINKB_SRC_BLOCK_TYPES = 0, ///< 8x8 块类型
    BINKB_SRC_COLORS,          ///< 用于不同块类型的像素值
    BINKB_SRC_PATTERN,         ///< 用于双色图案填充的 8 位值
    BINKB_SRC_X_OFF,           ///< 运动矢量的 X 分量
    BINKB_SRC_Y_OFF,           ///< 运动矢量的 Y 分量
    BINKB_SRC_INTRA_DC,        ///< 带 DCT 的帧内块的 DC 值
    BINKB_SRC_INTER_DC,        ///< 带 DCT 的帧间块的 DC 值
    BINKB_SRC_INTRA_Q,         ///< 带 DCT 的帧内块的量化器值
    BINKB_SRC_INTER_Q,         ///< 带 DCT 的帧间块的量化器值
    BINKB_SRC_INTER_COEFS,     ///< 残差块的系数个数

    BINKB_NB_SRC
};

static const int binkb_bundle_sizes[BINKB_NB_SRC] = {
    4, 8, 8, 5, 5, 11, 11, 4, 4, 7
};

static const int binkb_bundle_signed[BINKB_NB_SRC] = {
    0, 0, 0, 1, 1, 0, 1, 0, 0, 0
};

static int32_t binkb_intra_quant[16][64];
static int32_t binkb_inter_quant[16][64];

/**
 * Bink 视频块类型
 */
enum BlockTypes {
    SKIP_BLOCK = 0, ///< 跳过的块
    SCALED_BLOCK,   ///< 尺寸为 16x16 的块
    MOTION_BLOCK,   ///< 以一定偏移从上一帧复制的块
    RUN_BLOCK,      ///< 由自定义扫描顺序的颜色行程构成的块
    RESIDUE_BLOCK,  ///< 叠加了部分差异值的运动块
    INTRA_BLOCK,    ///< 帧内 DCT 块
    FILL_BLOCK,     ///< 用单一颜色填充的块
    INTER_BLOCK,    ///< 对差值应用 DCT 的运动块
    PATTERN_BLOCK,  ///< 按照自定义图案用两种颜色填充的块
    RAW_BLOCK,      ///< 未编码的 8x8 块
};

static const uint8_t bink_rlelens[4] = { 4, 8, 12, 32 };

/** 用于存储束中第一个 DC 值的位数 */
#define DC_START_BITS 11

/* ------------------------------------------------------------------ */
/* 解码器上下文                                                      */
/* ------------------------------------------------------------------ */

struct BinkVideo {
    int width, height;      /*!< 亮度尺寸                     */
    int version;            /*!< 内部 Bink 文件版本（'b'..'k'） */
    int has_alpha;
    int swap_planes;
    unsigned frame_num;
    int nb_planes;          /*!< 3 或 4（含 alpha）          */

    /* 当前帧与参考帧平面 */
    uint8_t *cur[4];
    uint8_t *last[4];
    int      stride[4];
    int      plane_w[4], plane_h[4];
    int      has_last;

    Bundle   bundle[BINKB_NB_SRC]; ///< 用于解码所有数据类型的束
    Tree     col_high[16];         ///< 用于解码 "colours" 数据类型中高半字节的树
    int      col_lastval;          ///< "colours" 数据类型中最后一个已解码高半字节的值

    /* 计算得到的静态表 */
    VLC      bink_trees[16];
    int      trees_built;
    int      binkb_initialised;
};

/* ------------------------------------------------------------------ */
/* 简单 DSP 原语（blockdsp + hpeldsp 的等价实现）                    */
/* ------------------------------------------------------------------ */

static void clear_block_c(int16_t *block)
{
    memset(block, 0, sizeof(int16_t) * 64);
}

static void fill_block16_c(uint8_t *block, uint8_t value, int line_size, int h)
{
    int i;
    for (i = 0; i < h; i++) {
        memset(block, value, 16);
        block += line_size;
    }
}

static void fill_block8_c(uint8_t *block, uint8_t value, int line_size, int h)
{
    int i;
    for (i = 0; i < h; i++) {
        memset(block, value, 8);
        block += line_size;
    }
}

/* put_pixels8x8（hpeldsp put_pixels_tab[1][0] 的 C 版本） */
static void put_pixels8(uint8_t *dst, const uint8_t *src, int stride, int h)
{
    int i;
    for (i = 0; i < h; i++) {
        memcpy(dst, src, 8);
        dst += stride;
        src += stride;
    }
}

/* ------------------------------------------------------------------ */
/* Bink IDCT（binkdsp.c）                                            */
/* ------------------------------------------------------------------ */

#define A1  2896 /* (1/sqrt(2))<<12 */
#define A2  2217
#define A3  3784
#define A4 -5352

#define MUL(X,Y) ((int)((unsigned)(X) * (Y)) >> 11)

#define IDCT_TRANSFORM(dest,s0,s1,s2,s3,s4,s5,s6,s7,d0,d1,d2,d3,d4,d5,d6,d7,munge,src) {\
    const int a0 = (src)[s0] + (src)[s4]; \
    const int a1 = (src)[s0] - (src)[s4]; \
    const int a2 = (src)[s2] + (src)[s6]; \
    const int a3 = MUL(A1, (src)[s2] - (src)[s6]); \
    const int a4 = (src)[s5] + (src)[s3]; \
    const int a5 = (src)[s5] - (src)[s3]; \
    const int a6 = (src)[s1] + (src)[s7]; \
    const int a7 = (src)[s1] - (src)[s7]; \
    const int b0 = a4 + a6; \
    const int b1 = MUL(A3, a5 + a7); \
    const int b2 = MUL(A4, a5) - b0 + b1; \
    const int b3 = MUL(A1, a6 - a4) - b2; \
    const int b4 = MUL(A2, a7) + b3 - b1; \
    (dest)[d0] = munge(a0+a2   +b0); \
    (dest)[d1] = munge(a1+a3-a2+b2); \
    (dest)[d2] = munge(a1-a3+a2+b3); \
    (dest)[d3] = munge(a0-a2   -b4); \
    (dest)[d4] = munge(a0-a2   +b4); \
    (dest)[d5] = munge(a1-a3+a2-b3); \
    (dest)[d6] = munge(a1+a3-a2-b2); \
    (dest)[d7] = munge(a0+a2   -b0); \
}
/* IDCT_TRANSFORM 宏结束 */

#define MUNGE_NONE(x) (x)
#define IDCT_COL(dest,src) IDCT_TRANSFORM(dest,0,8,16,24,32,40,48,56,0,8,16,24,32,40,48,56,MUNGE_NONE,src)

#define MUNGE_ROW(x) (((x) + 0x7F)>>8)
#define IDCT_ROW(dest,src) IDCT_TRANSFORM(dest,0,1,2,3,4,5,6,7,0,1,2,3,4,5,6,7,MUNGE_ROW,src)

static inline void bink_idct_col(int *dest, const int32_t *src)
{
    if ((src[8]|src[16]|src[24]|src[32]|src[40]|src[48]|src[56])==0) {
        dest[0]  =
        dest[8]  =
        dest[16] =
        dest[24] =
        dest[32] =
        dest[40] =
        dest[48] =
        dest[56] = src[0];
    } else {
        IDCT_COL(dest, src);
    }
}

static void bink_idct_c(int32_t *block)
{
    int i;
    int temp[64];

    for (i = 0; i < 8; i++)
        bink_idct_col(&temp[i], &block[i]);
    for (i = 0; i < 8; i++) {
        IDCT_ROW( (&block[8*i]), (&temp[8*i]) );
    }
}

static void bink_idct_add_c(uint8_t *dest, int linesize, int32_t *block)
{
    int i, j;

    bink_idct_c(block);
    for (i = 0; i < 8; i++, dest += linesize, block += 8)
        for (j = 0; j < 8; j++)
             dest[j] += block[j];
}

static void bink_idct_put_c(uint8_t *dest, int linesize, int32_t *block)
{
    int i;
    int temp[64];
    for (i = 0; i < 8; i++)
        bink_idct_col(&temp[i], &block[i]);
    for (i = 0; i < 8; i++) {
        IDCT_ROW( (&dest[i*linesize]), (&temp[8*i]) );
    }
}

static void scale_block_c(const uint8_t src[64], uint8_t *dst, int linesize)
{
    int i, j;
    uint16_t *dst1 = (uint16_t *) dst;
    uint16_t *dst2 = (uint16_t *)(dst + linesize);

    for (j = 0; j < 8; j++) {
        for (i = 0; i < 8; i++) {
            dst1[i] = dst2[i] = src[i] * 0x0101;
        }
        src  += 8;
        dst1 += linesize;
        dst2 += linesize;
    }
}

static void add_pixels8_c(uint8_t *pixels, int16_t *block, int line_size)
{
    int i;

    for (i = 0; i < 8; i++) {
        pixels[0] += block[0];
        pixels[1] += block[1];
        pixels[2] += block[2];
        pixels[3] += block[3];
        pixels[4] += block[4];
        pixels[5] += block[5];
        pixels[6] += block[6];
        pixels[7] += block[7];
        pixels    += line_size;
        block     += 8;
    }
}

/* ------------------------------------------------------------------ */
/* Bundle 辅助函数                                                    */
/* ------------------------------------------------------------------ */

/**
 * 初始化所有束的长度。
 */
static void init_lengths(BinkVideo *v, int width, int bw)
{
    width = FFALIGN(width, 8);

    v->bundle[BINK_SRC_BLOCK_TYPES].len = av_log2((width >> 3) + 511) + 1;
    v->bundle[BINK_SRC_SUB_BLOCK_TYPES].len = av_log2((width >> 4) + 511) + 1;
    v->bundle[BINK_SRC_COLORS].len = av_log2(bw*64 + 511) + 1;

    v->bundle[BINK_SRC_INTRA_DC].len =
    v->bundle[BINK_SRC_INTER_DC].len =
    v->bundle[BINK_SRC_X_OFF].len =
    v->bundle[BINK_SRC_Y_OFF].len = av_log2((width >> 3) + 511) + 1;

    v->bundle[BINK_SRC_PATTERN].len = av_log2((bw << 3) + 511) + 1;
    v->bundle[BINK_SRC_RUN].len = av_log2(bw*48 + 511) + 1;
}

/**
 * 为束分配内存。
 */
static int init_bundles(BinkVideo *v)
{
    int bw, bh, blocks;
    uint8_t *tmp;
    int i;

    bw = (v->width  + 7) >> 3;
    bh = (v->height + 7) >> 3;
    blocks = bw * bh;

    tmp = (uint8_t *)bink_calloc(blocks, 64 * BINKB_NB_SRC);
    if (!tmp)
        return BINK_ERROR_ENOMEM;
    for (i = 0; i < BINKB_NB_SRC; i++) {
        v->bundle[i].data     = tmp;
        tmp                  += blocks * 64;
        v->bundle[i].data_end = tmp;
    }

    return 0;
}

static void free_bundles(BinkVideo *v)
{
    bink_freep(&v->bundle[0].data);
}

/**
 * 根据读取的位合并两个大小相同的连续列表。
 */
static void merge(BitContext *gb, uint8_t *dst, uint8_t *src, int size)
{
    uint8_t *src2 = src + size;
    int size2 = size;

    do {
        if (!get_bits1(gb)) {
            *dst++ = *src++;
            size--;
        } else {
            *dst++ = *src2++;
            size2--;
        }
    } while (size && size2);

    while (size--)
        *dst++ = *src++;
    while (size2--)
        *dst++ = *src2++;
}

/**
 * 读取用于解码数据的 Huffman 树信息。
 */
static int read_tree(BitContext *gb, Tree *tree)
{
    uint8_t tmp1[16] = { 0 }, tmp2[16], *in = tmp1, *out = tmp2;
    int i, t, len;

    if (get_bits_left(gb) < 4)
        return BINK_ERROR_INVALIDDATA;

    tree->vlc_num = get_bits(gb, 4);
    if (!tree->vlc_num) {
        for (i = 0; i < 16; i++)
            tree->syms[i] = i;
        return 0;
    }
    if (get_bits1(gb)) {
        len = get_bits(gb, 3);
        for (i = 0; i <= len; i++) {
            tree->syms[i] = get_bits(gb, 4);
            tmp1[tree->syms[i]] = 1;
        }
        for (i = 0; i < 16 && len < 16 - 1; i++)
            if (!tmp1[i])
                tree->syms[++len] = i;
    } else {
        len = get_bits(gb, 2);
        for (i = 0; i < 16; i++)
            in[i] = i;
        for (i = 0; i <= len; i++) {
            int size = 1 << i;
            for (t = 0; t < 16; t += size << 1)
                merge(gb, out + t, in + t, size);
            {
                uint8_t *swp = in;
                in  = out;
                out = swp;
            }
        }
        memcpy(tree->syms, in, 16);
    }
    return 0;
}

/**
 * 为解码数据准备束。
 */
static int read_bundle(BitContext *gb, BinkVideo *v, int bundle_num)
{
    int i;

    if (bundle_num == BINK_SRC_COLORS) {
        for (i = 0; i < 16; i++) {
            int ret = read_tree(gb, &v->col_high[i]);
            if (ret < 0)
                return ret;
        }
        v->col_lastval = 0;
    }
    if (bundle_num != BINK_SRC_INTRA_DC && bundle_num != BINK_SRC_INTER_DC) {
        int ret = read_tree(gb, &v->bundle[bundle_num].tree);
        if (ret < 0)
            return ret;
    }
    v->bundle[bundle_num].cur_dec =
    v->bundle[bundle_num].cur_ptr = v->bundle[bundle_num].data;

    return 0;
}

#define CHECK_READ_VAL(gb, b, t) \
    if (!b->cur_dec || (b->cur_dec > b->cur_ptr)) \
        return 0; \
    t = get_bits(gb, b->len); \
    if (!t) { \
        b->cur_dec = NULL; \
        return 0; \
    } \

static int read_runs(BinkVideo *v, BitContext *gb, Bundle *b)
{
    int t, vv;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end) {
        bink_log(BINK_LOG_ERROR, "Run value went out of bounds\n");
        return BINK_ERROR_INVALIDDATA;
    }
    if (get_bits_left(gb) < 1)
        return BINK_ERROR_INVALIDDATA;
    if (get_bits1(gb)) {
        vv = get_bits(gb, 4);
        memset(b->cur_dec, vv, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end)
            *b->cur_dec++ = GET_HUFF(gb, b->tree);
    }
    return 0;
}

static int read_motion_values(BinkVideo *v, BitContext *gb, Bundle *b)
{
    int t, sign, vv;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end) {
        bink_log(BINK_LOG_ERROR, "Too many motion values\n");
        return BINK_ERROR_INVALIDDATA;
    }
    if (get_bits_left(gb) < 1)
        return BINK_ERROR_INVALIDDATA;
    if (get_bits1(gb)) {
        vv = get_bits(gb, 4);
        if (vv) {
            sign = -get_bits1(gb);
            vv = (vv ^ sign) - sign;
        }
        memset(b->cur_dec, vv, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            vv = GET_HUFF(gb, b->tree);
            if (vv) {
                sign = -get_bits1(gb);
                vv = (vv ^ sign) - sign;
            }
            *b->cur_dec++ = vv;
        }
    }
    return 0;
}

static int read_block_types(BinkVideo *v, BitContext *gb, Bundle *b)
{
    int t, vv;
    int last = 0;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    if (v->version == 'k') {
        t ^= 0xBBu;
        if (t == 0) {
            b->cur_dec = NULL;
            return 0;
        }
    }
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end) {
        bink_log(BINK_LOG_ERROR, "Too many block type values\n");
        return BINK_ERROR_INVALIDDATA;
    }
    if (get_bits_left(gb) < 1)
        return BINK_ERROR_INVALIDDATA;
    if (get_bits1(gb)) {
        vv = get_bits(gb, 4);
        memset(b->cur_dec, vv, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            vv = GET_HUFF(gb, b->tree);
            if (vv > 15) {
                bink_log(BINK_LOG_ERROR, "[DBG] block_types bad vv=%d t=%d bits=%d\n", vv, t, get_bits_count(gb));
                return BINK_ERROR_INVALIDDATA;
            }
            if (vv < 12) {
                last = vv;
                *b->cur_dec++ = vv;
            } else {
                int run = bink_rlelens[vv - 12];

                if (dec_end - b->cur_dec < run) {
                    bink_log(BINK_LOG_ERROR, "[DBG] block_types run=%d overrun rem=%d t=%d bits=%d\n",
                             run, (int)(dec_end - b->cur_dec), t, get_bits_count(gb));
                    return BINK_ERROR_INVALIDDATA;
                }
                memset(b->cur_dec, last, run);
                b->cur_dec += run;
            }
        }
    }
    return 0;
}

static int read_patterns(BinkVideo *v, BitContext *gb, Bundle *b)
{
    int t, vv;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end) {
        bink_log(BINK_LOG_ERROR, "Too many pattern values\n");
        return BINK_ERROR_INVALIDDATA;
    }
    while (b->cur_dec < dec_end) {
        if (get_bits_left(gb) < 2)
            return BINK_ERROR_INVALIDDATA;
        vv  = GET_HUFF(gb, b->tree);
        vv |= GET_HUFF(gb, b->tree) << 4;
        *b->cur_dec++ = vv;
    }

    return 0;
}

static int read_colors(BitContext *gb, Bundle *b, BinkVideo *v)
{
    int t, sign, vv;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end) {
        bink_log(BINK_LOG_ERROR, "Too many color values\n");
        return BINK_ERROR_INVALIDDATA;
    }
    if (get_bits_left(gb) < 1)
        return BINK_ERROR_INVALIDDATA;
    if (get_bits1(gb)) {
        v->col_lastval = GET_HUFF(gb, v->col_high[v->col_lastval]);
        vv = GET_HUFF(gb, b->tree);
        vv = (v->col_lastval << 4) | vv;
        if (v->version < 'i') {
            sign = ((int8_t) vv) >> 7;
            vv = ((vv & 0x7F) ^ sign) - sign;
            vv += 0x80;
        }
        memset(b->cur_dec, vv, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            if (get_bits_left(gb) < 2)
                return BINK_ERROR_INVALIDDATA;
            v->col_lastval = GET_HUFF(gb, v->col_high[v->col_lastval]);
            vv = GET_HUFF(gb, b->tree);
            vv = (v->col_lastval << 4) | vv;
            if (v->version < 'i') {
                sign = ((int8_t) vv) >> 7;
                vv = ((vv & 0x7F) ^ sign) - sign;
                vv += 0x80;
            }
            *b->cur_dec++ = vv;
        }
    }
    return 0;
}

static int read_dcs(BinkVideo *v, BitContext *gb, Bundle *b,
                    int start_bits, int has_sign)
{
    int i, j, len, len2, bsize, sign, vv, v2;
    int16_t *dst     = (int16_t*)b->cur_dec;
    int16_t *dst_end = (int16_t*)b->data_end;

    (void)v;
    CHECK_READ_VAL(gb, b, len);
    if (get_bits_left(gb) < start_bits - has_sign)
        return BINK_ERROR_INVALIDDATA;
    vv = get_bits(gb, start_bits - has_sign);
    if (vv && has_sign) {
        sign = -get_bits1(gb);
        vv = (vv ^ sign) - sign;
    }
    if (dst_end - dst < 1)
        return BINK_ERROR_INVALIDDATA;
    *dst++ = vv;
    len--;
    for (i = 0; i < len; i += 8) {
        len2 = FFMIN(len - i, 8);
        if (dst_end - dst < len2)
            return BINK_ERROR_INVALIDDATA;
        bsize = get_bits(gb, 4);
        if (bsize) {
            for (j = 0; j < len2; j++) {
                v2 = get_bits(gb, bsize);
                if (v2) {
                    sign = -get_bits1(gb);
                    v2 = (v2 ^ sign) - sign;
                }
                vv += v2;
                *dst++ = vv;
                if (vv < -32768 || vv > 32767) {
                    bink_log(BINK_LOG_ERROR, "DC value went out of bounds: %d\n", vv);
                    return BINK_ERROR_INVALIDDATA;
                }
            }
        } else {
            for (j = 0; j < len2; j++)
                *dst++ = vv;
        }
    }

    b->cur_dec = (uint8_t*)dst;
    return 0;
}

/**
 * 从束中获取下一个值。
 */
static inline int get_value(BinkVideo *v, int bundle)
{
    int ret;

    if (bundle < BINK_SRC_X_OFF || bundle == BINK_SRC_RUN)
        return *v->bundle[bundle].cur_ptr++;
    if (bundle == BINK_SRC_X_OFF || bundle == BINK_SRC_Y_OFF)
        return (int8_t)*v->bundle[bundle].cur_ptr++;
    ret = *(int16_t*)v->bundle[bundle].cur_ptr;
    v->bundle[bundle].cur_ptr += 2;
    return ret;
}

static void binkb_init_bundle(BinkVideo *v, int bundle_num)
{
    v->bundle[bundle_num].cur_dec =
    v->bundle[bundle_num].cur_ptr = v->bundle[bundle_num].data;
    v->bundle[bundle_num].len = 13;
}

static void binkb_init_bundles(BinkVideo *v)
{
    int i;
    for (i = 0; i < BINKB_NB_SRC; i++)
        binkb_init_bundle(v, i);
}

static int binkb_read_bundle(BinkVideo *v, BitContext *gb, int bundle_num)
{
    const int bits = binkb_bundle_sizes[bundle_num];
    const int mask = 1 << (bits - 1);
    const int issigned = binkb_bundle_signed[bundle_num];
    Bundle *b = &v->bundle[bundle_num];
    int i, len;

    CHECK_READ_VAL(gb, b, len);
    if (b->data_end - b->cur_dec < len * (1 + (bits > 8)))
        return BINK_ERROR_INVALIDDATA;
    if (bits <= 8) {
        if (!issigned) {
            for (i = 0; i < len; i++)
                *b->cur_dec++ = get_bits(gb, bits);
        } else {
            for (i = 0; i < len; i++)
                *b->cur_dec++ = get_bits(gb, bits) - mask;
        }
    } else {
        int16_t *dst = (int16_t*)b->cur_dec;

        if (!issigned) {
            for (i = 0; i < len; i++)
                *dst++ = get_bits(gb, bits);
        } else {
            for (i = 0; i < len; i++)
                *dst++ = get_bits(gb, bits) - mask;
        }
        b->cur_dec = (uint8_t*)dst;
    }
    return 0;
}

static inline int binkb_get_value(BinkVideo *v, int bundle_num)
{
    int16_t ret;
    const int bits = binkb_bundle_sizes[bundle_num];

    if (bits <= 8) {
        int val = *v->bundle[bundle_num].cur_ptr++;
        return binkb_bundle_signed[bundle_num] ? (int8_t)val : val;
    }
    ret = *(int16_t*)v->bundle[bundle_num].cur_ptr;
    v->bundle[bundle_num].cur_ptr += 2;
    return ret;
}

/* ------------------------------------------------------------------ */
/* DCT 系数解码                                                      */
/* ------------------------------------------------------------------ */

/**
 * 读取 8x8 块的 DCT 系数。
 */
static int read_dct_coeffs(BinkVideo *v, BitContext *gb, int32_t block[64],
                           const uint8_t *scan, int *coef_count_,
                           int coef_idx[64], int q)
{
    int coef_list[128];
    int mode_list[128];
    int i, t, bits, ccoef, mode, sign;
    int list_start = 64, list_end = 64, list_pos;
    int coef_count = 0;
    int quant_idx;

    (void)v;
    if (get_bits_left(gb) < 4)
        return BINK_ERROR_INVALIDDATA;

    coef_list[list_end] = 4;  mode_list[list_end++] = 0;
    coef_list[list_end] = 24; mode_list[list_end++] = 0;
    coef_list[list_end] = 44; mode_list[list_end++] = 0;
    coef_list[list_end] = 1;  mode_list[list_end++] = 3;
    coef_list[list_end] = 2;  mode_list[list_end++] = 3;
    coef_list[list_end] = 3;  mode_list[list_end++] = 3;

    for (bits = get_bits(gb, 4) - 1; bits >= 0; bits--) {
        list_pos = list_start;
        while (list_pos < list_end) {
            if (!(mode_list[list_pos] | coef_list[list_pos]) || !get_bits1(gb)) {
                list_pos++;
                continue;
            }
            ccoef = coef_list[list_pos];
            mode  = mode_list[list_pos];
            switch (mode) {
            case 0:
                coef_list[list_pos] = ccoef + 4;
                mode_list[list_pos] = 1;
                /* fall through */
            case 2:
                if (mode == 2) {
                    coef_list[list_pos]   = 0;
                    mode_list[list_pos++] = 0;
                }
                for (i = 0; i < 4; i++, ccoef++) {
                    if (get_bits1(gb)) {
                        coef_list[--list_start] = ccoef;
                        mode_list[  list_start] = 3;
                    } else {
                        if (!bits) {
                            t = 1 - (get_bits1(gb) << 1);
                        } else {
                            t = get_bits(gb, bits) | 1 << bits;
                            sign = -get_bits1(gb);
                            t = (t ^ sign) - sign;
                        }
                        block[scan[ccoef]] = t;
                        coef_idx[coef_count++] = ccoef;
                    }
                }
                break;
            case 1:
                mode_list[list_pos] = 2;
                for (i = 0; i < 3; i++) {
                    ccoef += 4;
                    coef_list[list_end]   = ccoef;
                    mode_list[list_end++] = 2;
                }
                break;
            case 3:
                if (!bits) {
                    t = 1 - (get_bits1(gb) << 1);
                } else {
                    t = get_bits(gb, bits) | 1 << bits;
                    sign = -get_bits1(gb);
                    t = (t ^ sign) - sign;
                }
                block[scan[ccoef]] = t;
                coef_idx[coef_count++] = ccoef;
                coef_list[list_pos]   = 0;
                mode_list[list_pos++] = 0;
                break;
            }
        }
    }

    if (q == -1) {
        quant_idx = get_bits(gb, 4);
    } else {
        quant_idx = q;
        if (quant_idx > 15) {
            bink_log(BINK_LOG_ERROR, "quant_index %d out of range\n", quant_idx);
            return BINK_ERROR_INVALIDDATA;
        }
    }

    *coef_count_ = coef_count;

    return quant_idx;
}

static void unquantize_dct_coeffs(int32_t block[64], const int32_t quant[64],
                                  int coef_count, int coef_idx[64],
                                  const uint8_t *scan)
{
    int i;
    block[0] = (int)(block[0] * quant[0]) >> 11;
    for (i = 0; i < coef_count; i++) {
        int idx = coef_idx[i];
        block[scan[idx]] = (int)(block[scan[idx]] * quant[idx]) >> 11;
    }
}

/**
 * 读取运动补偿后带残差的 8x8 块。
 */
static int read_residue(BitContext *gb, int16_t block[64], int masks_count)
{
    int coef_list[128];
    int mode_list[128];
    int i, sign, mask, ccoef, mode;
    int list_start = 64, list_end = 64, list_pos;
    int nz_coeff[64];
    int nz_coeff_count = 0;

    coef_list[list_end] =  4; mode_list[list_end++] = 0;
    coef_list[list_end] = 24; mode_list[list_end++] = 0;
    coef_list[list_end] = 44; mode_list[list_end++] = 0;
    coef_list[list_end] =  0; mode_list[list_end++] = 2;

    for (mask = 1 << get_bits(gb, 3); mask; mask >>= 1) {
        for (i = 0; i < nz_coeff_count; i++) {
            if (!get_bits1(gb))
                continue;
            if (block[nz_coeff[i]] < 0)
                block[nz_coeff[i]] -= mask;
            else
                block[nz_coeff[i]] += mask;
            masks_count--;
            if (masks_count < 0)
                return 0;
        }
        list_pos = list_start;
        while (list_pos < list_end) {
            if (!(coef_list[list_pos] | mode_list[list_pos]) || !get_bits1(gb)) {
                list_pos++;
                continue;
            }
            ccoef = coef_list[list_pos];
            mode  = mode_list[list_pos];
            switch (mode) {
            case 0:
                coef_list[list_pos] = ccoef + 4;
                mode_list[list_pos] = 1;
                /* fall through */
            case 2:
                if (mode == 2) {
                    coef_list[list_pos]   = 0;
                    mode_list[list_pos++] = 0;
                }
                for (i = 0; i < 4; i++, ccoef++) {
                    if (get_bits1(gb)) {
                        coef_list[--list_start] = ccoef;
                        mode_list[  list_start] = 3;
                    } else {
                        nz_coeff[nz_coeff_count++] = bink_scan[ccoef];
                        sign = -get_bits1(gb);
                        block[bink_scan[ccoef]] = (mask ^ sign) - sign;
                        masks_count--;
                        if (masks_count < 0)
                            return 0;
                    }
                }
                break;
            case 1:
                mode_list[list_pos] = 2;
                for (i = 0; i < 3; i++) {
                    ccoef += 4;
                    coef_list[list_end]   = ccoef;
                    mode_list[list_end++] = 2;
                }
                break;
            case 3:
                nz_coeff[nz_coeff_count++] = bink_scan[ccoef];
                sign = -get_bits1(gb);
                block[bink_scan[ccoef]] = (mask ^ sign) - sign;
                coef_list[list_pos]   = 0;
                mode_list[list_pos++] = 0;
                masks_count--;
                if (masks_count < 0)
                    return 0;
                break;
            }
        }
    }

    return 0;
}

/**
 * 将 8x8 块从源复制到目标，其中 src 和 dst 可能重叠
 */
static inline void put_pixels8x8_overlapped(uint8_t *dst, uint8_t *src, int stride)
{
    uint8_t tmp[64];
    int i;
    for (i = 0; i < 8; i++)
        memcpy(tmp + i*8, src + i*stride, 8);
    for (i = 0; i < 8; i++)
        memcpy(dst + i*stride, tmp + i*8, 8);
}

/* ------------------------------------------------------------------ */
/* 平面解码器                                                        */
/* ------------------------------------------------------------------ */

static int binkb_decode_plane(BinkVideo *v, BitContext *gb,
                              int plane_idx, int is_key, int is_chroma)
{
    int blk, ret;
    int i, j, bx, by;
    uint8_t *dst, *ref, *ref_start, *ref_end;
    int vv, col[2];
    const uint8_t *scan;
    int xoff, yoff;
    BINK_ALIGNED_32 int16_t block[64];
    BINK_ALIGNED_16 int32_t dctblock[64];
    int coordmap[64];
    int ybias = is_key ? -15 : 0;
    int qp, quant_idx, coef_count, coef_idx[64];

    const int stride = v->stride[plane_idx];
    int bw = is_chroma ? (v->width  + 15) >> 4 : (v->width  + 7) >> 3;
    int bh = is_chroma ? (v->height + 15) >> 4 : (v->height + 7) >> 3;

    binkb_init_bundles(v);
    ref_start = v->cur[plane_idx];
    ref_end   = v->cur[plane_idx] + ((bh - 1) * v->stride[plane_idx] + bw - 1) * 8;

    for (i = 0; i < 64; i++)
        coordmap[i] = (i & 7) + (i >> 3) * stride;

    for (by = 0; by < bh; by++) {
        for (i = 0; i < BINKB_NB_SRC; i++) {
            if ((ret = binkb_read_bundle(v, gb, i)) < 0)
                return ret;
        }

        dst  = v->cur[plane_idx] + 8*by*stride;
        for (bx = 0; bx < bw; bx++, dst += 8) {
            blk = binkb_get_value(v, BINKB_SRC_BLOCK_TYPES);
            switch (blk) {
            case 0:
                break;
            case 1:
                scan = bink_patterns[get_bits(gb, 4)];
                i = 0;
                do {
                    int mode, run;

                    mode = get_bits1(gb);
                    run = get_bits(gb, binkb_runbits[i]) + 1;

                    i += run;
                    if (i > 64) {
                        bink_log(BINK_LOG_ERROR, "Run went out of bounds\n");
                        return BINK_ERROR_INVALIDDATA;
                    }
                    if (mode) {
                        vv = binkb_get_value(v, BINKB_SRC_COLORS);
                        for (j = 0; j < run; j++)
                            dst[coordmap[*scan++]] = vv;
                    } else {
                        for (j = 0; j < run; j++)
                            dst[coordmap[*scan++]] = binkb_get_value(v, BINKB_SRC_COLORS);
                    }
                } while (i < 63);
                if (i == 63)
                    dst[coordmap[*scan++]] = binkb_get_value(v, BINKB_SRC_COLORS);
                break;
            case 2:
                memset(dctblock, 0, sizeof(*dctblock) * 64);
                dctblock[0] = binkb_get_value(v, BINKB_SRC_INTRA_DC);
                qp = binkb_get_value(v, BINKB_SRC_INTRA_Q);
                if ((quant_idx = read_dct_coeffs(v, gb, dctblock, bink_scan, &coef_count, coef_idx, qp)) < 0)
                    return quant_idx;
                unquantize_dct_coeffs(dctblock, binkb_intra_quant[quant_idx], coef_count, coef_idx, bink_scan);
                bink_idct_put_c(dst, stride, dctblock);
                break;
            case 3:
                xoff = binkb_get_value(v, BINKB_SRC_X_OFF);
                yoff = binkb_get_value(v, BINKB_SRC_Y_OFF) + ybias;
                ref = dst + xoff + yoff * stride;
                if (ref < ref_start || ref > ref_end) {
                    bink_log(BINK_LOG_WARNING, "Reference block is out of bounds\n");
                } else if (ref + 8*stride < dst || ref >= dst + 8*stride) {
                    put_pixels8(dst, ref, stride, 8);
                } else {
                    put_pixels8x8_overlapped(dst, ref, stride);
                }
                clear_block_c(block);
                vv = binkb_get_value(v, BINKB_SRC_INTER_COEFS);
                read_residue(gb, block, vv);
                add_pixels8_c(dst, block, stride);
                break;
            case 4:
                xoff = binkb_get_value(v, BINKB_SRC_X_OFF);
                yoff = binkb_get_value(v, BINKB_SRC_Y_OFF) + ybias;
                ref = dst + xoff + yoff * stride;
                if (ref < ref_start || ref > ref_end) {
                    bink_log(BINK_LOG_WARNING, "Reference block is out of bounds\n");
                } else if (ref + 8*stride < dst || ref >= dst + 8*stride) {
                    put_pixels8(dst, ref, stride, 8);
                } else {
                    put_pixels8x8_overlapped(dst, ref, stride);
                }
                memset(dctblock, 0, sizeof(*dctblock) * 64);
                dctblock[0] = binkb_get_value(v, BINKB_SRC_INTER_DC);
                qp = binkb_get_value(v, BINKB_SRC_INTER_Q);
                if ((quant_idx = read_dct_coeffs(v, gb, dctblock, bink_scan, &coef_count, coef_idx, qp)) < 0)
                    return quant_idx;
                unquantize_dct_coeffs(dctblock, binkb_inter_quant[quant_idx], coef_count, coef_idx, bink_scan);
                bink_idct_add_c(dst, stride, dctblock);
                break;
            case 5:
                vv = binkb_get_value(v, BINKB_SRC_COLORS);
                fill_block8_c(dst, vv, stride, 8);
                break;
            case 6:
                for (i = 0; i < 2; i++)
                    col[i] = binkb_get_value(v, BINKB_SRC_COLORS);
                for (i = 0; i < 8; i++) {
                    vv = binkb_get_value(v, BINKB_SRC_PATTERN);
                    for (j = 0; j < 8; j++, vv >>= 1)
                        dst[i*stride + j] = col[vv & 1];
                }
                break;
            case 7:
                xoff = binkb_get_value(v, BINKB_SRC_X_OFF);
                yoff = binkb_get_value(v, BINKB_SRC_Y_OFF) + ybias;
                ref = dst + xoff + yoff * stride;
                if (ref < ref_start || ref > ref_end) {
                    bink_log(BINK_LOG_WARNING, "Reference block is out of bounds\n");
                } else if (ref + 8*stride < dst || ref >= dst + 8*stride) {
                    put_pixels8(dst, ref, stride, 8);
                } else {
                    put_pixels8x8_overlapped(dst, ref, stride);
                }
                break;
            case 8:
                for (i = 0; i < 8; i++)
                    memcpy(dst + i*stride, v->bundle[BINKB_SRC_COLORS].cur_ptr + i*8, 8);
                v->bundle[BINKB_SRC_COLORS].cur_ptr += 64;
                break;
            default:
                bink_log(BINK_LOG_ERROR, "Unknown block type %d\n", blk);
                return BINK_ERROR_INVALIDDATA;
            }
        }
    }
    if (get_bits_count(gb) & 0x1F) /* 下一个平面的数据从 32 位边界开始 */
        skip_bits_long(gb, 32 - (get_bits_count(gb) & 0x1F));

    return 0;
}

static int bink_put_pixels(BinkVideo *v,
                           uint8_t *dst, uint8_t *prev, int stride,
                           uint8_t *ref_start,
                           uint8_t *ref_end)
{
    int xoff     = get_value(v, BINK_SRC_X_OFF);
    int yoff     = get_value(v, BINK_SRC_Y_OFF);
    uint8_t *ref = prev + xoff + yoff * stride;
    if (ref < ref_start || ref > ref_end) {
        bink_log(BINK_LOG_ERROR, "Copy out of bounds @%d, %d\n",
                 xoff, yoff);
        return BINK_ERROR_INVALIDDATA;
    }
    put_pixels8(dst, ref, stride, 8);

    return 0;
}

static int bink_decode_plane(BinkVideo *v, BitContext *gb,
                             int plane_idx, int is_chroma)
{
    int blk, ret;
    int i, j, bx, by;
    uint8_t *dst, *prev, *ref_start, *ref_end;
    int vv, col[2];
    const uint8_t *scan;
    BINK_ALIGNED_32 int16_t block[64];
    BINK_ALIGNED_16 uint8_t ublock[64];
    BINK_ALIGNED_16 int32_t dctblock[64];
    int coordmap[64], quant_idx, coef_count, coef_idx[64];

    const int stride = v->stride[plane_idx];
    int bw = is_chroma ? (v->width  + 15) >> 4 : (v->width  + 7) >> 3;
    int bh = is_chroma ? (v->height + 15) >> 4 : (v->height + 7) >> 3;
    int width = v->width >> is_chroma;
    int height = v->height >> is_chroma;

    if (v->version == 'k' && get_bits1(gb)) {
        int fill = get_bits(gb, 8);

        dst = v->cur[plane_idx];

        for (i = 0; i < height; i++)
            memset(dst + i * stride, fill, width);
        goto end;
    }

    init_lengths(v, FFMAX(width, 8), bw);
    for (i = 0; i < BINK_NB_SRC; i++) {
        ret = read_bundle(gb, v, i);
        if (ret < 0) {
            bink_log(BINK_LOG_ERROR,
                     "[DBG] plane=%d read_bundle[%d] failed ret=%d bits=%d/%d\n",
                     plane_idx, i, ret, get_bits_count(gb),
                     gb->size_in_bits);
            return ret;
        }
    }
    {
        int last_stride = v->has_last ? v->stride[plane_idx] : 0;
        ref_start = v->has_last ? v->last[plane_idx] : v->cur[plane_idx];
        ref_end   = ref_start + (bw - 1 + last_stride * (bh - 1)) * 8;
    }

    for (i = 0; i < 64; i++)
        coordmap[i] = (i & 7) + (i >> 3) * stride;

    for (by = 0; by < bh; by++) {
        if ((ret = read_block_types(v, gb, &v->bundle[BINK_SRC_BLOCK_TYPES])) < 0) {
            bink_log(BINK_LOG_ERROR, "[DBG] by=%d read_block_types failed %d bits=%d\n", by, ret, get_bits_count(gb));
            return ret;
        }
        if ((ret = read_block_types(v, gb, &v->bundle[BINK_SRC_SUB_BLOCK_TYPES])) < 0) {
            bink_log(BINK_LOG_ERROR, "[DBG] by=%d read_sub failed %d bits=%d\n", by, ret, get_bits_count(gb));
            return ret;
        }
        if ((ret = read_colors(gb, &v->bundle[BINK_SRC_COLORS], v)) < 0) {
            bink_log(BINK_LOG_ERROR, "[DBG] by=%d read_colors failed %d bits=%d\n", by, ret, get_bits_count(gb));
            return ret;
        }
        if ((ret = read_patterns(v, gb, &v->bundle[BINK_SRC_PATTERN])) < 0) {
            bink_log(BINK_LOG_ERROR, "[DBG] by=%d read_patterns failed %d bits=%d\n", by, ret, get_bits_count(gb));
            return ret;
        }
        if ((ret = read_motion_values(v, gb, &v->bundle[BINK_SRC_X_OFF])) < 0) {
            bink_log(BINK_LOG_ERROR, "[DBG] by=%d read_xoff failed %d bits=%d\n", by, ret, get_bits_count(gb));
            return ret;
        }
        if ((ret = read_motion_values(v, gb, &v->bundle[BINK_SRC_Y_OFF])) < 0) {
            bink_log(BINK_LOG_ERROR, "[DBG] by=%d read_yoff failed %d bits=%d\n", by, ret, get_bits_count(gb));
            return ret;
        }
        if ((ret = read_dcs(v, gb, &v->bundle[BINK_SRC_INTRA_DC], DC_START_BITS, 0)) < 0) {
            bink_log(BINK_LOG_ERROR, "[DBG] by=%d read_intra_dc failed %d bits=%d\n", by, ret, get_bits_count(gb));
            return ret;
        }
        if ((ret = read_dcs(v, gb, &v->bundle[BINK_SRC_INTER_DC], DC_START_BITS, 1)) < 0) {
            bink_log(BINK_LOG_ERROR, "[DBG] by=%d read_inter_dc failed %d bits=%d\n", by, ret, get_bits_count(gb));
            return ret;
        }
        if ((ret = read_runs(v, gb, &v->bundle[BINK_SRC_RUN])) < 0) {
            bink_log(BINK_LOG_ERROR, "[DBG] by=%d read_runs failed %d bits=%d\n", by, ret, get_bits_count(gb));
            return ret;
        }

        dst  = v->cur[plane_idx] + 8*by*stride;
        prev = (v->has_last ? v->last[plane_idx] : v->cur[plane_idx]) + 8*by*stride;
        for (bx = 0; bx < bw; bx++, dst += 8, prev += 8) {
            blk = get_value(v, BINK_SRC_BLOCK_TYPES);
            /* 奇数行上的 16x16 块类型表示其属于已解码块的一部分，因此跳过 */
            if (((by & 1) || (bx & 1)) && blk == SCALED_BLOCK) {
                bx++;
                dst  += 8;
                prev += 8;
                continue;
            }
            switch (blk) {
            case SKIP_BLOCK:
                put_pixels8(dst, prev, stride, 8);
                break;
            case SCALED_BLOCK:
                blk = get_value(v, BINK_SRC_SUB_BLOCK_TYPES);
                switch (blk) {
                case RUN_BLOCK:
                    if (get_bits_left(gb) < 4)
                        return BINK_ERROR_INVALIDDATA;
                    scan = bink_patterns[get_bits(gb, 4)];
                    i = 0;
                    do {
                        int run = get_value(v, BINK_SRC_RUN) + 1;

                        i += run;
                        if (i > 64) {
                            bink_log(BINK_LOG_ERROR, "Run went out of bounds\n");
                            return BINK_ERROR_INVALIDDATA;
                        }
                        if (get_bits1(gb)) {
                            vv = get_value(v, BINK_SRC_COLORS);
                            for (j = 0; j < run; j++)
                                ublock[*scan++] = vv;
                        } else {
                            for (j = 0; j < run; j++)
                                ublock[*scan++] = get_value(v, BINK_SRC_COLORS);
                        }
                    } while (i < 63);
                    if (i == 63)
                        ublock[*scan++] = get_value(v, BINK_SRC_COLORS);
                    break;
                case INTRA_BLOCK:
                    memset(dctblock, 0, sizeof(*dctblock) * 64);
                    dctblock[0] = get_value(v, BINK_SRC_INTRA_DC);
                    if ((quant_idx = read_dct_coeffs(v, gb, dctblock, bink_scan, &coef_count, coef_idx, -1)) < 0)
                        return quant_idx;
                    unquantize_dct_coeffs(dctblock, bink_intra_quant[quant_idx], coef_count, coef_idx, bink_scan);
                    bink_idct_put_c(ublock, 8, dctblock);
                    break;
                case FILL_BLOCK:
                    vv = get_value(v, BINK_SRC_COLORS);
                    fill_block16_c(dst, vv, stride, 16);
                    break;
                case PATTERN_BLOCK:
                    for (i = 0; i < 2; i++)
                        col[i] = get_value(v, BINK_SRC_COLORS);
                    for (j = 0; j < 8; j++) {
                        vv = get_value(v, BINK_SRC_PATTERN);
                        for (i = 0; i < 8; i++, vv >>= 1)
                            ublock[i + j*8] = col[vv & 1];
                    }
                    break;
                case RAW_BLOCK:
                    for (j = 0; j < 8; j++)
                        for (i = 0; i < 8; i++)
                            ublock[i + j*8] = get_value(v, BINK_SRC_COLORS);
                    break;
                default:
                    bink_log(BINK_LOG_ERROR, "Incorrect 16x16 block type %d\n", blk);
                    return BINK_ERROR_INVALIDDATA;
                }
                if (blk != FILL_BLOCK)
                    scale_block_c(ublock, dst, stride);
                bx++;
                dst  += 8;
                prev += 8;
                break;
            case MOTION_BLOCK:
                ret = bink_put_pixels(v, dst, prev, stride,
                                      ref_start, ref_end);
                if (ret < 0)
                    return ret;
                break;
            case RUN_BLOCK:
                scan = bink_patterns[get_bits(gb, 4)];
                i = 0;
                do {
                    int run = get_value(v, BINK_SRC_RUN) + 1;

                    i += run;
                    if (i > 64) {
                        bink_log(BINK_LOG_ERROR, "Run went out of bounds\n");
                        return BINK_ERROR_INVALIDDATA;
                    }
                    if (get_bits1(gb)) {
                        vv = get_value(v, BINK_SRC_COLORS);
                        for (j = 0; j < run; j++)
                            dst[coordmap[*scan++]] = vv;
                    } else {
                        for (j = 0; j < run; j++)
                            dst[coordmap[*scan++]] = get_value(v, BINK_SRC_COLORS);
                    }
                } while (i < 63);
                if (i == 63)
                    dst[coordmap[*scan++]] = get_value(v, BINK_SRC_COLORS);
                break;
            case RESIDUE_BLOCK:
                ret = bink_put_pixels(v, dst, prev, stride,
                                      ref_start, ref_end);
                if (ret < 0)
                    return ret;
                clear_block_c(block);
                vv = get_bits(gb, 7);
                read_residue(gb, block, vv);
                add_pixels8_c(dst, block, stride);
                break;
            case INTRA_BLOCK:
                memset(dctblock, 0, sizeof(*dctblock) * 64);
                dctblock[0] = get_value(v, BINK_SRC_INTRA_DC);
                if ((quant_idx = read_dct_coeffs(v, gb, dctblock, bink_scan, &coef_count, coef_idx, -1)) < 0)
                    return quant_idx;
                unquantize_dct_coeffs(dctblock, bink_intra_quant[quant_idx], coef_count, coef_idx, bink_scan);
                bink_idct_put_c(dst, stride, dctblock);
                break;
            case FILL_BLOCK:
                vv = get_value(v, BINK_SRC_COLORS);
                fill_block8_c(dst, vv, stride, 8);
                break;
            case INTER_BLOCK:
                ret = bink_put_pixels(v, dst, prev, stride,
                                      ref_start, ref_end);
                if (ret < 0)
                    return ret;
                memset(dctblock, 0, sizeof(*dctblock) * 64);
                dctblock[0] = get_value(v, BINK_SRC_INTER_DC);
                if ((quant_idx = read_dct_coeffs(v, gb, dctblock, bink_scan, &coef_count, coef_idx, -1)) < 0)
                    return quant_idx;
                unquantize_dct_coeffs(dctblock, bink_inter_quant[quant_idx], coef_count, coef_idx, bink_scan);
                bink_idct_add_c(dst, stride, dctblock);
                break;
            case PATTERN_BLOCK:
                for (i = 0; i < 2; i++)
                    col[i] = get_value(v, BINK_SRC_COLORS);
                for (i = 0; i < 8; i++) {
                    vv = get_value(v, BINK_SRC_PATTERN);
                    for (j = 0; j < 8; j++, vv >>= 1)
                        dst[i*stride + j] = col[vv & 1];
                }
                break;
            case RAW_BLOCK:
                for (i = 0; i < 8; i++)
                    memcpy(dst + i*stride, v->bundle[BINK_SRC_COLORS].cur_ptr + i*8, 8);
                v->bundle[BINK_SRC_COLORS].cur_ptr += 64;
                break;
            default:
                bink_log(BINK_LOG_ERROR, "Unknown block type %d\n", blk);
                return BINK_ERROR_INVALIDDATA;
            }
        }
    }

end:
    if (get_bits_count(gb) & 0x1F) /* 下一个平面的数据从 32 位边界开始 */
        skip_bits_long(gb, 32 - (get_bits_count(gb) & 0x1F));

    return 0;
}

/* ------------------------------------------------------------------ */
/* 初始化 / 量化表                                                    */
/* ------------------------------------------------------------------ */

/**
 * 计算版本 b 的量化表
 */
static void binkb_calc_quant(BinkVideo *v)
{
    uint8_t inv_bink_scan[64];
    static const int s[64]={
        1073741824,1489322693,1402911301,1262586814,1073741824, 843633538, 581104888, 296244703,
        1489322693,2065749918,1945893874,1751258219,1489322693,1170153332, 806015634, 410903207,
        1402911301,1945893874,1832991949,1649649171,1402911301,1102260336, 759250125, 387062357,
        1262586814,1751258219,1649649171,1484645031,1262586814, 992008094, 683307060, 348346918,
        1073741824,1489322693,1402911301,1262586814,1073741824, 843633538, 581104888, 296244703,
         843633538,1170153332,1102260336, 992008094, 843633538, 662838617, 456571181, 232757969,
         581104888, 806015634, 759250125, 683307060, 581104888, 456571181, 314491699, 160326478,
         296244703, 410903207, 387062357, 348346918, 296244703, 232757969, 160326478,  81733730,
    };
    int i, j;
#define C (1LL<<30)
    for (i = 0; i < 64; i++)
        inv_bink_scan[bink_scan[i]] = i;

    for (j = 0; j < 16; j++) {
        for (i = 0; i < 64; i++) {
            int k = inv_bink_scan[i];
            binkb_intra_quant[j][k] = binkb_intra_seed[i] * (int64_t)s[i] *
                                        binkb_num[j]/(binkb_den[j] * (C>>12));
            binkb_inter_quant[j][k] = binkb_inter_seed[i] * (int64_t)s[i] *
                                        binkb_num[j]/(binkb_den[j] * (C>>12));
        }
    }
#undef C
    (void)v;
}

static int bink_build_trees(BinkVideo *v)
{
    static VLC_TYPE table[16 * 128][2];
    int i;

    if (v->trees_built)
        return 0;
    for (i = 0; i < 16; i++) {
        const int maxbits = bink_tree_lens[i][15];
        v->bink_trees[i].table = table + i*128;
        v->bink_trees[i].table_allocated = 1 << maxbits;
        /* bink_tree_bits 以 little-endian（LSB 优先）位顺序存储 Huffman 码；
           INIT_VLC_LE 使 build_table 以 MSB 优先的位读取器（bink_get_vlc2）
           能够正确解码的方式布局查找表。
           这与 FFmpeg 的 `INIT_VLC_USE_NEW_STATIC | INIT_VLC_LE` 一致。 */
        if (bink_init_vlc(&v->bink_trees[i], maxbits, 16,
                          bink_tree_lens[i], 1, 1,
                          bink_tree_bits[i], 1, 1,
                          NULL, 0, 0,
                          INIT_VLC_USE_NEW_STATIC | INIT_VLC_LE) < 0)
            return BINK_ERROR_INVALIDDATA;
    }
    v->trees_built = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                          */
/* ------------------------------------------------------------------ */

int bink_video_init(BinkVideo **out, int width, int height,
                    const uint8_t *extradata, int extradata_size,
                    const char *codec_tag)
{
    BinkVideo *v;
    int i, ret;
    int flags = 0;

    if (!out || width <= 0 || height <= 0 || !codec_tag)
        return BINK_ERROR_EINVAL;

    v = (BinkVideo *)bink_calloc(1, sizeof(BinkVideo));
    if (!v)
        return BINK_ERROR_ENOMEM;

    v->width  = width;
    v->height = height;
    /* version 是编解码器标签的修订字符，例如 "BIKi" 中的 'i'。
       传入的标签字符串是文件顺序中的 4 个标签字节，
       因此修订字符是最后一个字符（codec_tag[3]）；
       这与 FFmpeg 的 `avctx->codec_tag >> 24` 一致。 */
    v->version = (unsigned char)codec_tag[3];
    if (extradata && extradata_size >= 4)
        flags = (int)bink_rl32(extradata);
    v->has_alpha  = flags & BINK_FLAG_ALPHA;
    v->swap_planes = v->version >= 'h';
    v->nb_planes  = v->has_alpha ? 4 : 3;

    /* 平面几何信息 */
    for (i = 0; i < 4; i++) {
        int pw, ph;
        if (i == 1 || i == 2) { /* 色度平面 */
            pw = (width  + 1) >> 1;
            ph = (height + 1) >> 1;
        } else {                /* 亮度 / alpha 平面 */
            pw = width;
            ph = height;
        }
        v->plane_w[i] = pw;
        v->plane_h[i] = ph;
        v->stride[i]  = FFALIGN(pw, 32);
        v->cur[i]  = (uint8_t *)bink_malloc((size_t)FFALIGN(ph, 8) * v->stride[i] + 64);
        v->last[i] = (uint8_t *)bink_malloc((size_t)FFALIGN(ph, 8) * v->stride[i] + 64);
        if (!v->cur[i] || !v->last[i]) {
            ret = BINK_ERROR_ENOMEM;
            goto fail;
        }
    }

    if ((ret = bink_build_trees(v)) < 0)
        goto fail;

    if ((ret = init_bundles(v)) < 0)
        goto fail;

    if (v->version == 'b') {
        if (!v->binkb_initialised) {
            binkb_calc_quant(v);
            v->binkb_initialised = 1;
        }
    }

    *out = v;
    return 0;

fail:
    bink_video_free(&v);
    return ret;
}

int bink_video_decode(BinkVideo *v, const uint8_t *pkt, int pkt_size,
                      int *keyframe)
{
    BitContext gb;
    int plane, plane_idx, ret;
    int bits_count = pkt_size << 3;

    if (!v || !pkt)
        return BINK_ERROR_EINVAL;

    bink_init_getbits(&gb, pkt, bits_count);
    /* ioEF: was BINK_LOG_ERROR upstream, which printed once per frame. */
    bink_log(BINK_LOG_DEBUG, "[DBG] video_decode pkt_size=%d version=%c has_alpha=%d\n",
             pkt_size, v->version, v->has_alpha);

    if (v->has_alpha) {
        if (v->version >= 'i')
            skip_bits_long(&gb, 32);
        if ((ret = bink_decode_plane(v, &gb, 3, 0)) < 0) {
            bink_log(BINK_LOG_ERROR, "[DBG] alpha plane failed ret=%d\n", ret);
            return ret;
        }
    }
    if (v->version >= 'i')
        skip_bits_long(&gb, 32);

    v->frame_num++;

    for (plane = 0; plane < 3; plane++) {
        plane_idx = (!plane || !v->swap_planes) ? plane : (plane ^ 3);

        if (v->version > 'b') {
            if ((ret = bink_decode_plane(v, &gb, plane_idx, !!plane)) < 0) {
                bink_log(BINK_LOG_ERROR, "[DBG] plane %d failed ret=%d bits=%d/%d\n",
                         plane_idx, ret, get_bits_count(&gb), bits_count);
                return ret;
            }
        } else {
            if ((ret = binkb_decode_plane(v, &gb, plane_idx,
                                          v->frame_num == 1, !!plane)) < 0) {
                bink_log(BINK_LOG_ERROR, "[DBG] binkb plane %d failed ret=%d\n",
                         plane_idx, ret);
                return ret;
            }
        }
        if (get_bits_count(&gb) >= bits_count)
            break;
    }

    /* 保留解码帧的副本以供下一帧参考 */
    if (v->version > 'b') {
        for (plane = 0; plane < v->nb_planes; plane++)
            memcpy(v->last[plane], v->cur[plane],
                   (size_t)FFALIGN(v->plane_h[plane], 8) * v->stride[plane]);
        v->has_last = 1;
    } else {
        /* 版本 'b' 就地解码到 cur；镜像到 last */
        for (plane = 0; plane < v->nb_planes; plane++)
            memcpy(v->last[plane], v->cur[plane],
                   (size_t)FFALIGN(v->plane_h[plane], 8) * v->stride[plane]);
        v->has_last = 1;
    }

    if (keyframe)
        *keyframe = (v->frame_num == 1);

    return 0;
}

void bink_video_get_frame(BinkVideo *v, BinkVideoFrame *frame)
{
    int i;
    if (!v || !frame)
        return;
    for (i = 0; i < v->nb_planes; i++) {
        frame->planes[i] = v->cur[i];
        frame->strides[i] = v->stride[i];
    }
    for (; i < 4; i++) {
        frame->planes[i] = NULL;
        frame->strides[i] = 0;
    }
    frame->width     = v->width;
    frame->height    = v->height;
    frame->has_alpha = v->has_alpha;
    frame->keyframe  = (v->frame_num == 1);
}

void bink_video_to_rgb(BinkVideo *v, uint8_t *rgb)
{
    const uint8_t *y, *u, *vv;
    int ys, us, vs;
    int x, ypos;
    BinkVideoFrame f;

    if (!v || !rgb)
        return;

    bink_video_get_frame(v, &f);
    y  = f.planes[0];  ys = f.strides[0];
    u  = f.planes[1];  us = f.strides[1];
    vv = f.planes[2];  vs = f.strides[2];

    for (ypos = 0; ypos < v->height; ypos++) {
        uint8_t *dst = rgb + (size_t)ypos * v->width * 3;
        const uint8_t *yl = y + (size_t)ypos * ys;
        const uint8_t *ul = u + (size_t)(ypos >> 1) * us;
        const uint8_t *vl = vv + (size_t)(ypos >> 1) * vs;
        for (x = 0; x < v->width; x++) {
            int yy = yl[x] - 16;
            int cb = ul[x >> 1] - 128;
            int cr = vl[x >> 1] - 128;
            int r, g, b;

            r = (298 * yy + 409 * cr + 128) >> 8;
            g = (298 * yy - 100 * cb - 208 * cr + 128) >> 8;
            b = (298 * yy + 516 * cb + 128) >> 8;

            dst[0] = (uint8_t)bink_clip(r, 0, 255);
            dst[1] = (uint8_t)bink_clip(g, 0, 255);
            dst[2] = (uint8_t)bink_clip(b, 0, 255);
            dst += 3;
        }
    }
}

void bink_video_free(BinkVideo **pv)
{
    BinkVideo *v;
    int i;
    if (!pv || !*pv)
        return;
    v = *pv;
    for (i = 0; i < 4; i++) {
        bink_free(v->cur[i]);
        bink_free(v->last[i]);
    }
    free_bundles(v);
    bink_free(v);
    *pv = NULL;
}

void bink_video_flush(BinkVideo *v)
{
    int i;

    if (!v)
        return;
    /* 清除参考帧，使 seek 之后的帧间帧不会基于 seek 之前的数据进行预测 */
    for (i = 0; i < 4; i++)
        memset(v->last[i], 0, (size_t)FFALIGN(v->plane_h[i], 8) * v->stride[i]);
    v->frame_num = 0;
    v->has_last  = 0;
}
