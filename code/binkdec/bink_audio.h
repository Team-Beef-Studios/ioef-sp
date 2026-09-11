/*
 * Bink decoder - standalone C port from FFmpeg
 *
 * This file is part of the standalone Bink decoder project (LGPL 2.1+).
 * See bink_common.h for the full license header.
 */

/**
 * @file bink_audio.h
 *
 * Bink 音频解码器。这是 FFmpeg 的 libavcodec/binkaudio.c 及其所依赖的
 * FFT / RDFT / DCT 变换（libavcodec/fft_template.c、rdft.c、dct.c）
 * 的无依赖移植。
 *
 * 支持两种编码变体，在初始化时选择：
 *   - BINK_AUDIO_DCT  ：基于 DCT-III（"Bink Audio (DCT)"，BIKa/BIKb）
 *   - BINK_AUDIO_RDFT ：基于逆实数 FFT（"Bink Audio (RDFT)"）
 */

#ifndef BINK_AUDIO_H
#define BINK_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BINK_AUDIO_DCT  0
#define BINK_AUDIO_RDFT 1
#define BINK_AUDIO_MAX_CHANNELS 2

typedef struct BinkAudio BinkAudio;

/**
 * 创建一个 Bink 音频解码器。
 *
 * @param out             接收新解码器（失败时为 NULL）
 * @param sample_rate     音频采样率，单位 Hz
 * @param channels        声道数（1 或 2）
 * @param use_dct         BINK_AUDIO_DCT 或 BINK_AUDIO_RDFT
 * @param extradata       编解码器附加数据；可为 NULL
 * @param extradata_size  附加数据大小
 * @return 成功返回 0，否则返回负错误码
 */
int bink_audio_init(BinkAudio **out, int sample_rate, int channels,
                    int use_dct, const uint8_t *extradata,
                    int extradata_size);

/**
 * 解码一个音频块。
 *
 * 每次调用为每个声道解码恰好一个包含 `frame_len` 个采样的块，写入调用者
 * 提供的 `out` 缓冲区（每个缓冲区至少需能容纳 `frame_len` 个 float），
 * 并通过 *out_samples 报告其中有多少是新增可用的采样
 * （等于 frame_len - overlap_len；对 RDFT 立体声变体为
 * (frame_len - overlap_len) / channels）。
 *
 * 数据包可能包含多个块：传入 `pkt` 为 NULL、`pkt_size` 为 0 可继续
 * 解码上一数据包剩余的块。当当前数据包完全消费完时返回 0。
 *
 * @param a           解码器
 * @param pkt         数据包载荷；为 NULL 表示继续上一数据包
 * @param pkt_size    数据包大小（字节）；pkt 为 NULL 时为 0
 * @param out         `channels` 个 float 缓冲区指针组成的数组，每个缓冲区
 *                    至少需能容纳 frame_len 个 float
 * @param out_samples 接收每个声道可用采样数（仅在成功时有意义）
 * @return 解码了一个块返回 1，数据包耗尽返回 0，否则返回负错误码
 */
int bink_audio_decode(BinkAudio *a, const uint8_t *pkt, int pkt_size,
                      float **out, int *out_samples);

/**
 * 返回帧长度（每声道变换尺寸，以采样计）以及每块可用采样数。
 */
void bink_audio_get_frame_info(BinkAudio *a, int *frame_len,
                               int *block_samples);

/**
 * 释放一个 Bink 音频解码器及其所有缓冲区。
 */
void bink_audio_free(BinkAudio **a);

/**
 * 重置解码器状态（重叠内存、位读取器）。在 seek 之后调用，以便
 * 重叠相加的历史不引用 seek 之前的采样。
 */
void bink_audio_flush(BinkAudio *a);

#ifdef __cplusplus
}
#endif

#endif /* BINK_AUDIO_H */
