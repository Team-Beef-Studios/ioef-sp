/*
 * Bink decoder - standalone C port from FFmpeg
 *
 * This file is part of the standalone Bink decoder project (LGPL 2.1+).
 * See bink_common.h for the full license header.
 */

/**
 * @file bink_bitstream.c
 *
 * VLC 表构建器 - FFmpeg 的 ff_init_vlc_sparse()/build_table()
 * （libavcodec/bitstream.c）的无依赖移植版本。
 */

#include "bink_bitstream.h"

#define LOCALBUF_ELEMS 1500 /*!< 可容纳在栈上的最大码数量 */

typedef struct BinkVLCcode {
    uint8_t bits;
    VLC_TYPE symbol;
    /** 码字，待读取的第一位位于最高位（msb）
     * （即使是为小端位流读取器准备的也是如此） */
    uint32_t code;
} BinkVLCcode;

/* 反转 32 位值的位顺序 */
static uint32_t bitswap_32(uint32_t x)
{
    x = ((x & 0x55555555) << 1) | ((x >> 1) & 0x55555555);
    x = ((x & 0x33333333) << 2) | ((x >> 2) & 0x33333333);
    x = ((x & 0x0F0F0F0F) << 4) | ((x >> 4) & 0x0F0F0F0F);
    x = ((x & 0x00FF00FF) << 8) | ((x >> 8) & 0x00FF00FF);
    return (x << 16) | (x >> 16);
}

#define GET_DATA(v, table, i, wrap, size)                   \
{                                                           \
    const uint8_t *ptr = (const uint8_t *)(table) + (i) * (wrap); \
    switch (size) {                                         \
    case 1: v = *(const uint8_t *)ptr; break;               \
    case 2: v = *(const uint16_t *)ptr; break;              \
    default: v = *(const uint32_t *)ptr; break;             \
    }                                                       \
}

static int bink_alloc_table(BinkVLC *vlc, int size, int use_static)
{
    int index = vlc->table_size;

    vlc->table_size += size;
    if (vlc->table_size > vlc->table_allocated) {
        if (use_static)
            return BINK_ERROR_EINVAL; /* 静态表太小 */
        vlc->table_allocated += (1 << vlc->bits);
        {
            BINK_VLC_TYPE *tmp = (BINK_VLC_TYPE *)bink_realloc(
                vlc->table, vlc->table_allocated * sizeof(BINK_VLC_TYPE) * 2);
            if (!tmp) {
                vlc->table_allocated = 0;
                vlc->table_size      = 0;
                return BINK_ERROR_ENOMEM;
            }
            vlc->table = (BINK_VLC_TYPE(*)[2])tmp;
        }
        memset(vlc->table + vlc->table_allocated - (1 << vlc->bits),
               0, sizeof(BINK_VLC_TYPE) * 2 << vlc->bits);
    }
    return index;
}

static int compare_vlcspec(const void *a, const void *b)
{
    const BinkVLCcode *sa = a, *sb = b;
    return (sa->code >> 1) - (sb->code >> 1);
}

/**
 * 构建适用于 bink_get_vlc2() 的 VLC 解码表。
 * 忠实移植自 FFmpeg 的 build_table()（libavcodec/bitstream.c）。
 */
static int bink_build_table(BinkVLC *vlc, int table_nb_bits, int nb_codes,
                            BinkVLCcode *codes, int flags)
{
    int table_size, table_index, index, code_prefix, symbol, subtable_bits;
    int i, j, k, n, nb, inc;
    uint32_t code;
    BINK_VLC_TYPE(*table)[2];

    if (table_nb_bits > 30)
        return BINK_ERROR_EINVAL;
    table_size = 1 << table_nb_bits;
    table_index = bink_alloc_table(vlc, table_size, flags & INIT_VLC_USE_NEW_STATIC);
    if (table_index < 0)
        return table_index;
    table = (BINK_VLC_TYPE(*)[2])&vlc->table[table_index];

    /* 第一遍：映射码并计算辅助表的大小 */
    for (i = 0; i < nb_codes; i++) {
        n      = codes[i].bits;
        code   = codes[i].code;
        symbol = codes[i].symbol;
        if (n <= table_nb_bits) {
            /* 无需再添加另一个表 */
            j  = code >> (32 - table_nb_bits);
            nb = 1 << (table_nb_bits - n);
            inc = 1;
            if (flags & INIT_VLC_OUTPUT_LE) {
                j   = bitswap_32(code);
                inc = 1 << n;
            }
            for (k = 0; k < nb; k++) {
                int bits  = table[j][1];
                int oldsym = table[j][0];
                if ((bits || oldsym) && (bits != n || oldsym != symbol))
                    return BINK_ERROR_INVALIDDATA;
                table[j][1] = n;       /* 位数 */
                table[j][0] = symbol;  /* 码 */
                j += inc;
            }
        } else {
            /* 递归填充辅助表 */
            n -= table_nb_bits;
            code_prefix = code >> (32 - table_nb_bits);
            subtable_bits = n;
            codes[i].bits = n;
            codes[i].code = code << table_nb_bits;
            for (k = i + 1; k < nb_codes; k++) {
                n = codes[k].bits - table_nb_bits;
                if (n <= 0)
                    break;
                code = codes[k].code;
                if (code >> (32 - table_nb_bits) != (uint32_t)code_prefix)
                    break;
                codes[k].bits = n;
                codes[k].code = code << table_nb_bits;
                subtable_bits = bink_max(subtable_bits, n);
            }
            subtable_bits = bink_min(subtable_bits, table_nb_bits);
            j = (flags & INIT_VLC_OUTPUT_LE) ?
                    bitswap_32(code_prefix) >> (32 - table_nb_bits) : (uint32_t)code_prefix;
            table[j][1] = -subtable_bits;
            index = bink_build_table(vlc, subtable_bits, k - i, codes + i, flags);
            if (index < 0)
                return index;
            /* 注意：realloc 已完成，因此需要重新加载表 */
            table = (BINK_VLC_TYPE(*)[2])&vlc->table[table_index];
            table[j][0] = index; /* 码 */
            i = k - 1;
        }
    }

    for (i = 0; i < table_size; i++) {
        if (table[i][1] == 0) /* 位数 */
            table[i][0] = -1; /* 码 */
    }

    return table_index;
}

static int bink_vlc_common_init(BinkVLC *vlc_arg, int nb_bits, int nb_codes,
                                BinkVLC **vlc, BinkVLC *localvlc,
                                BinkVLCcode **buf, int flags)
{
    *vlc = vlc_arg;
    (*vlc)->bits = nb_bits;
    if (flags & INIT_VLC_USE_NEW_STATIC) {
        *localvlc = *vlc_arg;
        *vlc = localvlc;
        (*vlc)->table_size = 0;
    } else {
        (*vlc)->table           = NULL;
        (*vlc)->table_allocated = 0;
        (*vlc)->table_size      = 0;
    }
    if (nb_codes > LOCALBUF_ELEMS) {
        *buf = (BinkVLCcode *)bink_malloc(nb_codes * sizeof(BinkVLCcode));
        if (!*buf)
            return BINK_ERROR_ENOMEM;
    }
    return 0;
}

static int bink_vlc_common_end(BinkVLC *vlc, int nb_bits, int nb_codes,
                               BinkVLCcode *codes, int flags,
                               BinkVLC *vlc_arg, BinkVLCcode localbuf[LOCALBUF_ELEMS])
{
    int ret = bink_build_table(vlc, nb_bits, nb_codes, codes, flags);

    if (flags & INIT_VLC_USE_NEW_STATIC) {
        if (vlc->table_size != vlc->table_allocated)
            bink_log(BINK_LOG_ERROR,
                     "needed %d had %d\n", vlc->table_size, vlc->table_allocated);
        if (ret < 0)
            return ret;
        *vlc_arg = *vlc;
    } else {
        if (codes != localbuf)
            bink_free(codes);
        if (ret < 0) {
            bink_freep(&vlc->table);
            return ret;
        }
    }
    return 0;
}

int bink_init_vlc(BinkVLC *vlc_arg, int nb_bits, int nb_codes,
                  const void *bits, int bits_wrap, int bits_size,
                  const void *codes, int codes_wrap, int codes_size,
                  const void *symbols, int symbols_wrap, int symbols_size,
                  int flags)
{
    BinkVLCcode localbuf[LOCALBUF_ELEMS], *buf = localbuf;
    int i, j, ret;
    BinkVLC localvlc, *vlc;

    ret = bink_vlc_common_init(vlc_arg, nb_bits, nb_codes, &vlc, &localvlc,
                               &buf, flags);
    if (ret < 0)
        return ret;

    j = 0;
#define COPY(condition)                                                         \
    for (i = 0; i < nb_codes; i++) {                                            \
        unsigned len;                                                           \
        GET_DATA(len, bits, i, bits_wrap, bits_size);                           \
        if (!(condition))                                                       \
            continue;                                                           \
        if (len > (unsigned)(3 * nb_bits) || len > 32) {                        \
            bink_log(BINK_LOG_ERROR, "Too long VLC (%u) in init_vlc\n", len);   \
            if (buf != localbuf)                                                \
                bink_free(buf);                                                 \
            return BINK_ERROR_EINVAL;                                           \
        }                                                                       \
        buf[j].bits = len;                                                      \
        GET_DATA(buf[j].code, codes, i, codes_wrap, codes_size);                \
        if (buf[j].code >= (1LL << buf[j].bits)) {                              \
            bink_log(BINK_LOG_ERROR, "Invalid code %x for %d in init_vlc\n",    \
                     buf[j].code, i);                                           \
            if (buf != localbuf)                                                \
                bink_free(buf);                                                 \
            return BINK_ERROR_EINVAL;                                           \
        }                                                                       \
        if (flags & INIT_VLC_INPUT_LE)                                          \
            buf[j].code = bitswap_32(buf[j].code);                              \
        else                                                                    \
            buf[j].code <<= 32 - buf[j].bits;                                   \
        if (symbols)                                                            \
            GET_DATA(buf[j].symbol, symbols, i, symbols_wrap, symbols_size)     \
        else                                                                    \
            buf[j].symbol = i;                                                  \
        j++;                                                                    \
    }
    COPY(len > (unsigned)nb_bits);
    qsort(buf, j, sizeof(BinkVLCcode), compare_vlcspec);
    COPY(len && len <= (unsigned)nb_bits);
    nb_codes = j;

    return bink_vlc_common_end(vlc, nb_bits, nb_codes, buf, flags, vlc_arg,
                               localbuf);
}

void bink_free_vlc(BinkVLC *vlc)
{
    bink_freep(&vlc->table);
}
