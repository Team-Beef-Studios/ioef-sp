/*
 * Bink decoder - standalone C port from FFmpeg
 *
 * Bink audio decoder
 * Copyright (c) 2007-2011 Peter Ross (pross@xvid.org)
 * Copyright (c) 2009 Daniel Verkamp (daniel@drv.nu)
 * (I)RDFT transforms
 * Copyright (c) 2009 Alex Converse <alex dot converse at gmail dot com>
 * (I)DCT transforms
 * Copyright (c) 2009 Peter Ross <pross@xvid.org>
 * Copyright (c) 2010 Alex Converse <alex.converse@gmail.com>
 * FFT/IFFT transforms
 * Copyright (c) 2008 Loren Merritt
 * Copyright (c) 2002 Fabrice Bellard
 *
 * This file is part of the standalone Bink decoder project (LGPL 2.1+).
 * See bink_common.h for the full license header.
 */

/**
 * @file bink_audio.c
 *
 * FFmpeg Bink 音频解码器的无依赖移植：
 *  - libavcodec/binkaudio.c （块解码、RLE、量化）
 *  - libavcodec/rdft.c      （逆实数 FFT）
 *  - libavcodec/dct.c       （DCT-III / DCT-II / DCT-I / DST-I）
 *  - libavcodec/fft_template.c （分裂基复数 FFT，float 版本）
 *  - libavcodec/wma_freqs.c （临界频带）
 *
 * 技术细节：
 *  http://wiki.multimedia.cx/index.php?title=Bink_Audio
 */

#include "bink_audio.h"
#include "bink_common.h"
#include "bink_bitstream.h"

#define BINK_AUDIO_MAX_CHANNELS 2
#define BINK_AUDIO_BLOCK_MAX_SIZE (BINK_AUDIO_MAX_CHANNELS << 11)

/* ================================================================== */
/* 余弦表（FFmpeg 的 ff_cos_tabs + ff_init_ff_cos_tabs）                  */
/* ================================================================== */

typedef float BinkFFTSample;
typedef float BinkFFTDouble;

/* bink_cos_tabs[k] 保存 cos(2*pi*x / 2^k)，x 取值 0..2^k/4，并附其
 * 反向排列（因此 bink_cos_tabs[k][x] == cos(2*pi*x / 2^k)，其中
 * x = 0..2^k/2-1）。仅使用索引 4..17。 */
static BinkFFTSample *bink_cos_tabs[18];

static void bink_init_ff_cos_tabs(int index)
{
    int i;
    int m = 1 << index;
    double freq = 2 * M_PI / m;
    BinkFFTSample *tab;

    if (index < 4 || index > 17)
        return;
    if (bink_cos_tabs[index])
        return; /* 已初始化 */
    tab = (BinkFFTSample *)bink_malloc((m / 2) * sizeof(BinkFFTSample));
    if (!tab)
        return;
    for (i = 0; i <= m / 4; i++)
        tab[i] = (BinkFFTSample)cos(i * freq);
    for (i = 1; i < m / 4; i++)
        tab[m / 2 - i] = tab[i];
    bink_cos_tabs[index] = tab;
}

/* ================================================================== */
/* 复数 FFT（FFmpeg 的 fft_template.c，FFT_FLOAT 版本）                     */
/* ================================================================== */

typedef struct BinkFFTComplex {
    BinkFFTSample re, im;
} BinkFFTComplex;

typedef struct BinkFFTContext {
    int nbits;
    int inverse;
    uint16_t *revtab;
    BinkFFTComplex *tmp_buf;
    void (*fft_permute)(struct BinkFFTContext *s, BinkFFTComplex *z);
    void (*fft_calc)(struct BinkFFTContext *s, BinkFFTComplex *z);
} BinkFFTContext;

#define BF(x, y, a, b) do {                     \
        x = a - b;                              \
        y = a + b;                              \
    } while (0)

#define CMUL(dre, dim, are, aim, bre, bim) do { \
        (dre) = (are) * (bre) - (aim) * (bim);  \
        (dim) = (are) * (bim) + (aim) * (bre);  \
    } while (0)

#define sqrthalf (float)M_SQRT1_2

static int split_radix_permutation(int i, int n, int inverse)
{
    int m;
    if (n <= 2)
        return i & 1;
    m = n >> 1;
    if (!(i & m))
        return split_radix_permutation(i, m, inverse) * 2;
    m >>= 1;
    if (inverse == !(i & m))
        return split_radix_permutation(i, m, inverse) * 4 + 1;
    else
        return split_radix_permutation(i, m, inverse) * 4 - 1;
}

static void fft_permute_c(BinkFFTContext *s, BinkFFTComplex *z);
static void fft_calc_c(BinkFFTContext *s, BinkFFTComplex *z);

static int bink_fft_init(BinkFFTContext *s, int nbits, int inverse)
{
    int i, j, n;

    s->revtab  = NULL;
    s->tmp_buf = NULL;

    if (nbits < 2 || nbits > 16)
        goto fail;
    s->nbits = nbits;
    n = 1 << nbits;

    s->revtab = (uint16_t *)bink_malloc(n * sizeof(uint16_t));
    if (!s->revtab)
        goto fail;
    s->tmp_buf = (BinkFFTComplex *)bink_malloc(n * sizeof(BinkFFTComplex));
    if (!s->tmp_buf)
        goto fail;
    s->inverse = inverse;
    s->fft_permute = fft_permute_c;
    s->fft_calc    = fft_calc_c;

    for (j = 4; j <= nbits; j++)
        bink_init_ff_cos_tabs(j);

    for (i = 0; i < n; i++) {
        /* FFmpeg 的标量路径（FF_FFT_PERM_DEFAULT）：revtab[k] = i。
         * SWAP_LSBS 变体仅适用于 AVX SIMD 蝶形运算，
         * 我们在此不使用。 */
        j = i;
        s->revtab[-split_radix_permutation(i, n, s->inverse) & (n - 1)] = j;
    }

    return 0;
fail:
    bink_freep(&s->revtab);
    bink_freep(&s->tmp_buf);
    return BINK_ERROR_EINVAL;
}

static void fft_permute_c(BinkFFTContext *s, BinkFFTComplex *z)
{
    int j, np;
    const uint16_t *revtab = s->revtab;
    np = 1 << s->nbits;
    for (j = 0; j < np; j++)
        s->tmp_buf[revtab[j]] = z[j];
    memcpy(z, s->tmp_buf, np * sizeof(BinkFFTComplex));
}

static void bink_fft_end(BinkFFTContext *s)
{
    bink_freep(&s->revtab);
    bink_freep(&s->tmp_buf);
}

#define BUTTERFLIES(a0,a1,a2,a3) {\
    BF(t3, t5, t5, t1);\
    BF(a2.re, a0.re, a0.re, t5);\
    BF(a3.im, a1.im, a1.im, t3);\
    BF(t4, t6, t2, t6);\
    BF(a3.re, a1.re, a1.re, t4);\
    BF(a2.im, a0.im, a0.im, t6);\
}

#define TRANSFORM(a0,a1,a2,a3,wre,wim) {\
    CMUL(t1, t2, a2.re, a2.im, wre, -wim);\
    CMUL(t5, t6, a3.re, a3.im, wre,  wim);\
    BUTTERFLIES(a0,a1,a2,a3)\
}

#define TRANSFORM_ZERO(a0,a1,a2,a3) {\
    t1 = a2.re;\
    t2 = a2.im;\
    t5 = a3.re;\
    t6 = a3.im;\
    BUTTERFLIES(a0,a1,a2,a3)\
}

/* z[0...8n-1]，w[1...2n-1] */
#define PASS(name)\
static void name(BinkFFTComplex *z, const BinkFFTSample *wre, unsigned int n)\
{\
    BinkFFTDouble t1, t2, t3, t4, t5, t6;\
    int o1 = 2*n;\
    int o2 = 4*n;\
    int o3 = 6*n;\
    const BinkFFTSample *wim = wre+o1;\
    n--;\
\
    TRANSFORM_ZERO(z[0],z[o1],z[o2],z[o3]);\
    TRANSFORM(z[1],z[o1+1],z[o2+1],z[o3+1],wre[1],wim[-1]);\
    do {\
        z += 2;\
        wre += 2;\
        wim -= 2;\
        TRANSFORM(z[0],z[o1],z[o2],z[o3],wre[0],wim[0]);\
        TRANSFORM(z[1],z[o1+1],z[o2+1],z[o3+1],wre[1],wim[-1]);\
    } while(--n);\
}

PASS(pass)

#define DECL_FFT(n,ti,n2,n4)\
static void fft##n(BinkFFTComplex *z)\
{\
    fft##n2(z);\
    fft##n4(z+n4*2);\
    fft##n4(z+n4*3);\
    pass(z, bink_cos_tabs[ti], n4/2);\
}

static void fft4(BinkFFTComplex *z)
{
    BinkFFTDouble t1, t2, t3, t4, t5, t6, t7, t8;

    BF(t3, t1, z[0].re, z[1].re);
    BF(t8, t6, z[3].re, z[2].re);
    BF(z[2].re, z[0].re, t1, t6);
    BF(t4, t2, z[0].im, z[1].im);
    BF(t7, t5, z[2].im, z[3].im);
    BF(z[3].im, z[1].im, t4, t8);
    BF(z[3].re, z[1].re, t3, t7);
    BF(z[2].im, z[0].im, t2, t5);
}

static void fft8(BinkFFTComplex *z)
{
    BinkFFTDouble t1, t2, t3, t4, t5, t6;

    fft4(z);

    BF(t1, z[5].re, z[4].re, -z[5].re);
    BF(t2, z[5].im, z[4].im, -z[5].im);
    BF(t5, z[7].re, z[6].re, -z[7].re);
    BF(t6, z[7].im, z[6].im, -z[7].im);

    BUTTERFLIES(z[0],z[2],z[4],z[6]);
    TRANSFORM(z[1],z[3],z[5],z[7],sqrthalf,sqrthalf);
}

static void fft16(BinkFFTComplex *z)
{
    BinkFFTDouble t1, t2, t3, t4, t5, t6;
    BinkFFTSample cos_16_1 = bink_cos_tabs[4][1];
    BinkFFTSample cos_16_3 = bink_cos_tabs[4][3];

    fft8(z);
    fft4(z + 8);
    fft4(z + 12);

    TRANSFORM_ZERO(z[0],z[4],z[8],z[12]);
    TRANSFORM(z[2],z[6],z[10],z[14],sqrthalf,sqrthalf);
    TRANSFORM(z[1],z[5],z[9],z[13],cos_16_1,cos_16_3);
    TRANSFORM(z[3],z[7],z[11],z[15],cos_16_3,cos_16_1);
}

DECL_FFT(32,5,16,8)
DECL_FFT(64,6,32,16)
DECL_FFT(128,7,64,32)
DECL_FFT(256,8,128,64)
DECL_FFT(512,9,256,128)
DECL_FFT(1024,10,512,256)
DECL_FFT(2048,11,1024,512)
DECL_FFT(4096,12,2048,1024)
DECL_FFT(8192,13,4096,2048)
DECL_FFT(16384,14,8192,4096)
DECL_FFT(32768,15,16384,8192)
DECL_FFT(65536,16,32768,16384)

static void (* const fft_dispatch[])(BinkFFTComplex *) = {
    fft4, fft8, fft16, fft32, fft64, fft128, fft256, fft512, fft1024,
    fft2048, fft4096, fft8192, fft16384, fft32768, fft65536
};

static void fft_calc_c(BinkFFTContext *s, BinkFFTComplex *z)
{
    fft_dispatch[s->nbits - 2](z);
}

/* ================================================================== */
/* 实数 FFT（FFmpeg 的 rdft.c）                                          */
/* ================================================================== */

enum BinkRDFTransformType {
    BINK_DFT_R2C,   /* 0 */
    BINK_IDFT_C2R,  /* 1 */
    BINK_IDFT_R2C,  /* 2 */
    BINK_DFT_C2R,   /* 3 */
};

typedef struct BinkRDFTContext {
    int nbits;
    int inverse;
    int sign_convention;

    /* 前/后旋转表 */
    const BinkFFTSample *tcos;
    const BinkFFTSample *tsin;
    int negative_sin;
    BinkFFTContext fft;
    void (*rdft_calc)(struct BinkRDFTContext *s, BinkFFTSample *z);
} BinkRDFTContext;

/** 将一个实数 FFT 映射为两个并行的实数奇偶 FFT，然后将这两个实数 FFT
 * 交织成一个复数 FFT，最后还原结果。
 * 参考：http://www.engineeringproductivitytools.com/stuff/T0001/PT10.HTM
 */
static void rdft_calc_c(BinkRDFTContext *s, BinkFFTSample *data)
{
    int i, i1, i2;
    BinkFFTComplex ev, od, odsum;
    const int n = 1 << s->nbits;
    const float k1 = 0.5;
    const float k2 = 0.5 - s->inverse;
    const BinkFFTSample *tcos = s->tcos;
    const BinkFFTSample *tsin = s->tsin;

    if (!s->inverse) {
        s->fft.fft_permute(&s->fft, (BinkFFTComplex *)data);
        s->fft.fft_calc(&s->fft, (BinkFFTComplex *)data);
    }
    /* i=0 是打包导致的特殊情况：DC 项为实数，因此我们将同样为实数的
       N/2 项与它合并处理。 */
    ev.re = data[0];
    data[0] = ev.re + data[1];
    data[1] = ev.re - data[1];

#define RDFT_UNMANGLE(sign0, sign1)                                         \
    for (i = 1; i < (n >> 2); i++) {                                        \
        i1 = 2 * i;                                                         \
        i2 = n - i1;                                                        \
        /* 分离偶 FFT 与奇 FFT */                                                  \
        ev.re =  k1 * (data[i1  ] + data[i2  ]);                            \
        od.im =  k2 * (data[i2  ] - data[i1  ]);                            \
        ev.im =  k1 * (data[i1+1] - data[i2+1]);                            \
        od.re =  k2 * (data[i1+1] + data[i2+1]);                            \
        /* 对奇 FFT 施加旋转因子，并加到偶 FFT 上 */                               \
        odsum.re = od.re * tcos[i] sign0 od.im * tsin[i];                   \
        odsum.im = od.im * tcos[i] sign1 od.re * tsin[i];                   \
        data[i1  ] =  ev.re + odsum.re;                                     \
        data[i1+1] =  ev.im + odsum.im;                                     \
        data[i2  ] =  ev.re - odsum.re;                                     \
        data[i2+1] =  odsum.im - ev.im;                                     \
    }

    if (s->negative_sin) {
        RDFT_UNMANGLE(+,-)
    } else {
        RDFT_UNMANGLE(-,+)
    }

    data[2*i+1] = s->sign_convention * data[2*i+1];
    if (s->inverse) {
        data[0] *= k1;
        data[1] *= k1;
        s->fft.fft_permute(&s->fft, (BinkFFTComplex *)data);
        s->fft.fft_calc(&s->fft, (BinkFFTComplex *)data);
    }
}

static int bink_rdft_init(BinkRDFTContext *s, int nbits, int trans)
{
    int n = 1 << nbits;
    int ret;

    s->nbits           = nbits;
    s->inverse         = trans == BINK_IDFT_C2R || trans == BINK_DFT_C2R;
    s->sign_convention = trans == BINK_IDFT_R2C || trans == BINK_DFT_C2R ? 1 : -1;
    s->negative_sin    = trans == BINK_DFT_C2R || trans == BINK_DFT_R2C;

    if (nbits < 4 || nbits > 16)
        return BINK_ERROR_EINVAL;

    if ((ret = bink_fft_init(&s->fft, nbits - 1,
                             trans == BINK_IDFT_C2R || trans == BINK_IDFT_R2C)) < 0)
        return ret;

    bink_init_ff_cos_tabs(nbits);
    s->tcos = bink_cos_tabs[nbits];
    s->tsin = bink_cos_tabs[nbits] + (n >> 2);
    s->rdft_calc = rdft_calc_c;

    return 0;
}

static void bink_rdft_end(BinkRDFTContext *s)
{
    bink_fft_end(&s->fft);
}

static inline void bink_rdft_calc(BinkRDFTContext *s, BinkFFTSample *z)
{
    s->rdft_calc(s, z);
}

/* ================================================================== */
/* DCT（FFmpeg 的 dct.c，不含专用的 32 点路径）                                 */
/* ================================================================== */

enum BinkDCTTransformType {
    BINK_DCT_II  = 0,
    BINK_DCT_III = 1,
    BINK_DCT_I   = 2,
    BINK_DST_I   = 3,
};

typedef struct BinkDCTContext {
    int nbits;
    int inverse;
    BinkRDFTContext rdft;
    const float *costab;
    BinkFFTSample *csc2;
    void (*dct_calc)(struct BinkDCTContext *s, BinkFFTSample *data);
} BinkDCTContext;

/* sin((M_PI * x / (2 * n)) */
#define SIN(s, n, x) (s->costab[(n) - (x)])

/* cos((M_PI * x / (2 * n)) */
#define COS(s, n, x) (s->costab[x])

static void dst_calc_I_c(BinkDCTContext *ctx, BinkFFTSample *data)
{
    int n = 1 << ctx->nbits;
    int i;

    data[0] = 0;
    for (i = 1; i < n / 2; i++) {
        float tmp1 = data[i    ];
        float tmp2 = data[n - i];
        float s    = SIN(ctx, n, 2 * i);

        s           *= tmp1 + tmp2;
        tmp1         = (tmp1 - tmp2) * 0.5f;
        data[i]      = s + tmp1;
        data[n - i]  = s - tmp1;
    }

    data[n / 2] *= 2;
    bink_rdft_calc(&ctx->rdft, data);

    data[0] *= 0.5f;

    for (i = 1; i < n - 2; i += 2) {
        data[i + 1] +=  data[i - 1];
        data[i]      = -data[i + 2];
    }

    data[n - 1] = 0;
}

static void dct_calc_I_c(BinkDCTContext *ctx, BinkFFTSample *data)
{
    int n = 1 << ctx->nbits;
    int i;
    float next = -0.5f * (data[0] - data[n]);

    for (i = 0; i < n / 2; i++) {
        float tmp1 = data[i];
        float tmp2 = data[n - i];
        float s    = SIN(ctx, n, 2 * i);
        float c    = COS(ctx, n, 2 * i);

        c *= tmp1 - tmp2;
        s *= tmp1 - tmp2;

        next += c;

        tmp1        = (tmp1 + tmp2) * 0.5f;
        data[i]     = tmp1 - s;
        data[n - i] = tmp1 + s;
    }

    bink_rdft_calc(&ctx->rdft, data);
    data[n] = data[1];
    data[1] = next;

    for (i = 3; i <= n; i += 2)
        data[i] = data[i - 2] - data[i];
}

static void dct_calc_III_c(BinkDCTContext *ctx, BinkFFTSample *data)
{
    int n = 1 << ctx->nbits;
    int i;

    float next  = data[n - 1];
    float inv_n = 1.0f / n;

    for (i = n - 2; i >= 2; i -= 2) {
        float val1 = data[i];
        float val2 = data[i - 1] - data[i + 1];
        float c    = COS(ctx, n, i);
        float s    = SIN(ctx, n, i);

        data[i]     = c * val1 + s * val2;
        data[i + 1] = s * val1 - c * val2;
    }

    data[1] = 2 * next;

    bink_rdft_calc(&ctx->rdft, data);

    for (i = 0; i < n / 2; i++) {
        float tmp1 = data[i]         * inv_n;
        float tmp2 = data[n - i - 1] * inv_n;
        float csc  = ctx->csc2[i] * (tmp1 - tmp2);

        tmp1            += tmp2;
        data[i]          = tmp1 + csc;
        data[n - i - 1]  = tmp1 - csc;
    }
}

static void dct_calc_II_c(BinkDCTContext *ctx, BinkFFTSample *data)
{
    int n = 1 << ctx->nbits;
    int i;
    float next;

    for (i = 0; i < n / 2; i++) {
        float tmp1 = data[i];
        float tmp2 = data[n - i - 1];
        float s    = SIN(ctx, n, 2 * i + 1);

        s    *= tmp1 - tmp2;
        tmp1  = (tmp1 + tmp2) * 0.5f;

        data[i]     = tmp1 + s;
        data[n-i-1] = tmp1 - s;
    }

    bink_rdft_calc(&ctx->rdft, data);

    next     = data[1] * 0.5;
    data[1] *= -1;

    for (i = n - 2; i >= 0; i -= 2) {
        float inr = data[i    ];
        float ini = data[i + 1];
        float c   = COS(ctx, n, i);
        float s   = SIN(ctx, n, i);

        data[i]     = c * inr + s * ini;
        data[i + 1] = next;

        next += s * inr - c * ini;
    }
}

static int bink_dct_init(BinkDCTContext *s, int nbits, int inverse)
{
    int n = 1 << nbits;
    int i;
    int ret;

    memset(s, 0, sizeof(*s));

    s->nbits   = nbits;
    s->inverse = inverse;

    bink_init_ff_cos_tabs(nbits + 2);

    s->costab = bink_cos_tabs[nbits + 2];
    s->csc2   = (BinkFFTSample *)bink_malloc((n / 2) * sizeof(BinkFFTSample));
    if (!s->csc2)
        return BINK_ERROR_ENOMEM;

    if ((ret = bink_rdft_init(&s->rdft, nbits, inverse == BINK_DCT_III)) < 0) {
        bink_freep(&s->csc2);
        return ret;
    }

    for (i = 0; i < n / 2; i++)
        s->csc2[i] = 0.5f / (BinkFFTSample)sin(M_PI / (2 * n) * (2 * i + 1));

    switch (inverse) {
    case BINK_DCT_I  : s->dct_calc = dct_calc_I_c;   break;
    case BINK_DCT_II : s->dct_calc = dct_calc_II_c;  break;
    case BINK_DCT_III: s->dct_calc = dct_calc_III_c; break;
    case BINK_DST_I  : s->dct_calc = dst_calc_I_c;   break;
    }

    return 0;
}

static void bink_dct_end(BinkDCTContext *s)
{
    bink_rdft_end(&s->rdft);
    bink_freep(&s->csc2);
}

static inline void bink_dct_calc(BinkDCTContext *s, BinkFFTSample *data)
{
    s->dct_calc(s, data);
}

/* ================================================================== */
/* 临界频带（FFmpeg 的 wma_freqs.c）                                       */
/* ================================================================== */

static const uint16_t bink_wma_critical_freqs[25] = {
      100,   200,  300,  400,  510,  630,   770,   920,
     1080,  1270, 1480, 1720, 2000, 2320,  2700,  3150,
     3700,  4400, 5300, 6400, 7700, 9500, 12000, 15500,
    24500,
};

/* ================================================================== */
/* Bink 音频解码器（FFmpeg 的 binkaudio.c）                                 */
/* ================================================================== */

typedef struct BinkAudio {
    int use_dct;            /* BINK_AUDIO_DCT 或 BINK_AUDIO_RDFT */
    int version_b;          /* Bink 版本 'b' */
    int first;
    int channels;           /* 解码器声道数（RDFT 为 1，DCT 为实际声道数） */
    int real_channels;      /* 流中声明的声道数 */
    int frame_len;          /* 变换尺寸（采样数） */
    int overlap_len;        /* 重叠尺寸（采样数） */
    int block_size;
    int num_bands;
    float root;
    unsigned int bands[26];
    float previous[BINK_AUDIO_MAX_CHANNELS][BINK_AUDIO_BLOCK_MAX_SIZE / 16];
    float quant_table[96];
    union {
        BinkRDFTContext rdft;
        BinkDCTContext dct;
    } trans;
    /* 每个声道一个解码块的内部临时存储 */
    float *tmp[BINK_AUDIO_MAX_CHANNELS];
    /* 当前数据包状态 */
    const uint8_t *pkt_data;
    int pkt_size;
    BitContext gb;
    int gb_valid;           /* 位读取器初始化后为非零 */
} BinkAudio;

static float bink_audio_get_float(BitContext *gb)
{
    int power = (int)get_bits(gb, 5);
    float f   = ldexpf((float)get_bits(gb, 23), power - 23);
    if (get_bits1(gb))
        f = -f;
    return f;
}

static const uint8_t rle_length_tab[16] = {
    2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64
};

static float bink_audio_int2float(uint32_t i)
{
    union { uint32_t i; float f; } u;
    u.i = i;
    return u.f;
}

/**
 * 解码一个 Bink 音频块。
 * @param[out] out 输出缓冲区，每个缓冲区可容纳 frame_len 个 float
 * @return 成功返回 0，失败返回负错误码
 */
static int decode_block(BinkAudio *s, float **out, int use_dct)
{
    int ch, i, j, k;
    float q, quant[25];
    int width, coeff;
    BitContext *gb = &s->gb;

    if (use_dct)
        skip_bits(gb, 2);

    for (ch = 0; ch < s->channels; ch++) {
        BinkFFTSample *coeffs = out[ch];

        if (s->version_b) {
            if (get_bits_left(gb) < 64)
                return BINK_ERROR_INVALIDDATA;
            coeffs[0] = bink_audio_int2float(get_bits_long(gb, 32)) * s->root;
            coeffs[1] = bink_audio_int2float(get_bits_long(gb, 32)) * s->root;
        } else {
            if (get_bits_left(gb) < 58)
                return BINK_ERROR_INVALIDDATA;
            coeffs[0] = bink_audio_get_float(gb) * s->root;
            coeffs[1] = bink_audio_get_float(gb) * s->root;
        }

        if (get_bits_left(gb) < s->num_bands * 8)
            return BINK_ERROR_INVALIDDATA;
        for (i = 0; i < s->num_bands; i++) {
            int value = (int)get_bits(gb, 8);
            quant[i]  = s->quant_table[bink_min(value, 95)];
        }

        k = 0;
        q = quant[0];

        /* 解析系数 */
        i = 2;
        while (i < s->frame_len) {
            if (s->version_b) {
                j = i + 16;
            } else {
                int v = (int)get_bits1(gb);
                if (v) {
                    v = (int)get_bits(gb, 4);
                    j = i + rle_length_tab[v] * 8;
                } else {
                    j = i + 8;
                }
            }

            j = bink_min(j, s->frame_len);

            width = (int)get_bits(gb, 4);
            if (width == 0) {
                memset(coeffs + i, 0, (j - i) * sizeof(*coeffs));
                i = j;
                while ((int)s->bands[k] < i)
                    q = quant[k++];
            } else {
                while (i < j) {
                    if ((int)s->bands[k] == i)
                        q = quant[k++];
                    coeff = (int)get_bits(gb, width);
                    if (coeff) {
                        int v;
                        v = (int)get_bits1(gb);
                        if (v)
                            coeffs[i] = -q * coeff;
                        else
                            coeffs[i] =  q * coeff;
                    } else {
                        coeffs[i] = 0.0f;
                    }
                    i++;
                }
            }
        }

        if (getenv("BINK_AUD_DUMP") && s->first) {
            int di;
            fprintf(stderr, "[AUD] ch=%d use_dct=%d num_bands=%d root=%.9g\n",
                    ch, use_dct, s->num_bands, s->root);
            for (di = 0; di < s->num_bands; di++)
                fprintf(stderr, "[AUD] band %d=%u q=%.9g\n", di,
                        s->bands[di], quant[di]);
            fprintf(stderr, "[AUD] dc=%.9g nyq=%.9g\n", coeffs[0], coeffs[1]);
            for (di = 2; di < s->frame_len; di++)
                fprintf(stderr, "[AUD] c[%d]=%.9g\n", di, coeffs[di]);
        }

        if (use_dct) {
            coeffs[0] /= 0.5;
            bink_dct_calc(&s->trans.dct, coeffs);
        } else {
            bink_rdft_calc(&s->trans.rdft, coeffs);
        }

        if (getenv("BINK_AUD_DUMP") && s->first) {
            FILE *f = fopen("our_post.bin", "wb");
            if (f) {
                fwrite(coeffs, sizeof(float), s->frame_len, f);
                fclose(f);
            }
        }
    }

    for (ch = 0; ch < s->channels; ch++) {
        int count = s->overlap_len * s->channels;
        if (!s->first) {
            j = ch;
            for (i = 0; i < s->overlap_len; i++, j += s->channels)
                out[ch][i] = (s->previous[ch][i] * (count - j) +
                                     out[ch][i] *          j) / count;
        }
        memcpy(s->previous[ch], &out[ch][s->frame_len - s->overlap_len],
               s->overlap_len * sizeof(*s->previous[ch]));
    }

    s->first = 0;

    return 0;
}

int bink_audio_init(BinkAudio **out, int sample_rate, int channels,
                    int use_dct, const uint8_t *extradata,
                    int extradata_size)
{
    BinkAudio *s;
    int sample_rate_half;
    int i, ret;
    int frame_len_bits;

    if (!out)
        return BINK_ERROR_EINVAL;
    *out = NULL;

    if (channels < 1 || channels > BINK_AUDIO_MAX_CHANNELS)
        return BINK_ERROR_INVALIDDATA;

    /* 确定帧长度 */
    if (sample_rate < 22050)
        frame_len_bits = 9;
    else if (sample_rate < 44100)
        frame_len_bits = 10;
    else
        frame_len_bits = 11;

    s = (BinkAudio *)bink_calloc(1, sizeof(BinkAudio));
    if (!s)
        return BINK_ERROR_ENOMEM;

    s->version_b = extradata && extradata_size >= 4 && extradata[3] == 'b';
    s->use_dct   = use_dct;
    s->real_channels = channels;

    if (!use_dct) {
        /* RDFT 变体：音频已交织 */
        sample_rate  *= channels;
        s->channels = 1;
        if (!s->version_b)
            frame_len_bits += bink_log2((unsigned)channels);
    } else {
        s->channels = channels;
    }

    s->frame_len   = 1 << frame_len_bits;
    s->overlap_len = s->frame_len / 16;
    s->block_size  = (s->frame_len - s->overlap_len) * s->channels;
    sample_rate_half = (sample_rate + 1LL) / 2;
    if (!use_dct)
        s->root = 2.0f / (float)(sqrt((double)s->frame_len) * 32768.0);
    else
        s->root = s->frame_len / (float)(sqrt((double)s->frame_len) * 32768.0);
    for (i = 0; i < 96; i++) {
        /* 该常数为 0.066399999/log10(M_E) 的结果 */
        s->quant_table[i] = expf(i * 0.15289164787221953823f) * s->root;
    }

    /* 计算频带数量 */
    for (s->num_bands = 1; s->num_bands < 25; s->num_bands++)
        if (sample_rate_half <= bink_wma_critical_freqs[s->num_bands - 1])
            break;

    /* 填充频带数据 */
    s->bands[0] = 2;
    for (i = 1; i < s->num_bands; i++)
        s->bands[i] = (bink_wma_critical_freqs[i - 1] * s->frame_len /
                       sample_rate_half) & ~1;
    s->bands[s->num_bands] = s->frame_len;

    s->first = 1;

    if (!use_dct)
        ret = bink_rdft_init(&s->trans.rdft, frame_len_bits, BINK_DFT_C2R);
    else
        ret = bink_dct_init(&s->trans.dct, frame_len_bits, BINK_DCT_III);
    if (ret < 0)
        goto fail;

    for (i = 0; i < BINK_AUDIO_MAX_CHANNELS; i++) {
        s->tmp[i] = (float *)bink_malloc(s->frame_len * sizeof(float));
        if (!s->tmp[i])
            goto fail;
    }

    *out = s;
    return 0;

fail:
    bink_audio_free(&s);
    return ret;
}

void bink_audio_get_frame_info(BinkAudio *a, int *frame_len,
                               int *block_samples)
{
    if (!a)
        return;
    if (frame_len)
        *frame_len = a->frame_len;
    if (block_samples)
        *block_samples = a->block_size / a->channels;
}

int bink_audio_decode(BinkAudio *a, const uint8_t *pkt, int pkt_size,
                      float **out, int *out_samples)
{
    int ret;
    int ch;
    int block_samples;

    if (!a || !out)
        return BINK_ERROR_EINVAL;

    if (pkt) {
        if (pkt_size < 4)
            return BINK_ERROR_INVALIDDATA;
        a->pkt_data = pkt;
        a->pkt_size = pkt_size;
        bink_init_getbits8(&a->gb, pkt, pkt_size);
        /* 跳过已声明的尺寸字段 */
        skip_bits_long(&a->gb, 32);
        a->gb_valid = 1;
    }

    if (!a->gb_valid)
        return BINK_ERROR_INVALIDDATA;

    if (get_bits_left(&a->gb) < 4)
        return 0; /* 数据包已耗尽 */

    ret = decode_block(a, a->tmp, a->use_dct);
    if (ret < 0)
        return ret;

    /* 将每个声道的可用部分拷贝给调用者。
       对于 RDFT 立体声变体，编码流是交织的，因此解码器在一个合并
       声道上工作；这里将其解交织到各声道的输出缓冲区中。 */
    block_samples = a->block_size / a->channels;
    if (!a->use_dct && a->real_channels > 1) {
        int frames = block_samples / a->real_channels;
        for (ch = 0; ch < a->real_channels; ch++) {
            int i;
            for (i = 0; i < frames; i++)
                out[ch][i] = a->tmp[0][i * a->real_channels + ch];
        }
        if (out_samples)
            *out_samples = frames;
    } else {
        for (ch = 0; ch < a->channels; ch++)
            memcpy(out[ch], a->tmp[ch], block_samples * sizeof(float));
        if (out_samples)
            *out_samples = block_samples;
    }

    get_bits_align32(&a->gb);
    if (!get_bits_left(&a->gb)) {
        a->gb_valid = 0;
        a->pkt_data = NULL;
    }

    return 1;
}

void bink_audio_free(BinkAudio **pa)
{
    BinkAudio *a;
    int i;

    if (!pa || !*pa)
        return;
    a = *pa;

    if (!a->use_dct)
        bink_rdft_end(&a->trans.rdft);
    else
        bink_dct_end(&a->trans.dct);

    for (i = 0; i < BINK_AUDIO_MAX_CHANNELS; i++)
        bink_freep(&a->tmp[i]);

    bink_free(a);
    *pa = NULL;
}

void bink_audio_flush(BinkAudio *a)
{
    int i;

    if (!a)
        return;
    a->first     = 1;
    a->gb_valid  = 0;
    a->pkt_data  = NULL;
    a->pkt_size  = 0;
    for (i = 0; i < BINK_AUDIO_MAX_CHANNELS; i++)
        memset(a->previous[i], 0, sizeof(a->previous[i]));
}
