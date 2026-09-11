/*
 * Bink decoder - standalone C port from FFmpeg
 *
 * This file is part of the standalone Bink decoder project (LGPL 2.1+).
 * See bink_common.h for the full license header.
 */

/**
 * @file bink_api.h
 *
 * 将解封装器、视频解码器和音频解码器整合在一起的高层 C API。
 * 典型用法如下：
 *
 *     BinkFile *f;
 *     bink_open(&f, "movie.bik");
 *     while (bink_decode_frame(f, &fr) > 0) {
 *         bink_frame_to_rgb(&fr, rgb, width * 3);   // 视频
 *         ... fr.audio[t] / fr.audio_nb_samples[t]  // 音频
 *     }
 *     bink_close(&f);
 */

#ifndef BINK_API_H
#define BINK_API_H

#include "bink_demux.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BinkFile BinkFile;

/**
 * 一帧视频的解码结果，包括属于该帧的音频数据包。
 * 所有指针都由 BinkFile 拥有，在下一次调用 bink_decode_frame() /
 * bink_seek() / bink_close() 之前始终有效。
 */
typedef struct BinkFrameResult {
    int64_t pts;            /*!< 视频帧索引 */
    int keyframe;           /*!< 关键帧为非零 */

    /* 视频平面（YUV420P，可包含 alpha 通道） */
    int width, height;
    int has_alpha;
    const uint8_t *planes[4];
    int strides[4];

    /* 从该视频帧的数据包中解码出的各音轨音频。
       样本为交错排列：audio[t][i*channels + c]。 */
    int nb_audio;
    int audio_channels[BINK_MAX_AUDIO_TRACKS];
    int audio_sample_rate[BINK_MAX_AUDIO_TRACKS];
    int audio_nb_samples[BINK_MAX_AUDIO_TRACKS]; /*!< 每声道样本数 */
    const float *audio[BINK_MAX_AUDIO_TRACKS];
} BinkFrameResult;

/**
 * 从磁盘打开一个 Bink 文件。
 *
 * @param f    接收句柄（失败时为 NULL）
 * @param path .bik 文件的路径
 * @return 成功返回 0，否则返回负错误码
 */
int bink_open(BinkFile **f, const char *path);

/**
 * 从内存缓冲区打开一个 Bink 容器。
 */
int bink_open_memory(BinkFile **f, const uint8_t *buf, int size);

/** 释放句柄及所有解码器缓冲区。 */
void bink_close(BinkFile **f);

/** 流数量（1 个视频流 + 音频轨）。 */
int bink_stream_count(BinkFile *f);

/** 只读的流信息；索引 0 为视频，1..N-1 为音频。 */
const BinkStreamInfo *bink_stream(BinkFile *f, int index);

/** 视频解码器可用时为 1。 */
int bink_has_video_decoder(BinkFile *f);

/**
 * 解码下一视频帧（以及其之前的音频数据包）。
 *
 * @param f  句柄
 * @param r  接收帧结果
 * @return 成功返回 1，流结束时返回 0，否则返回负错误码
 */
int bink_decode_frame(BinkFile *f, BinkFrameResult *r);

/**
 * 跳转到某一视频帧。中间帧依赖前一帧，因此跳转会落在
 * 所请求帧之前或该帧处最近的关键帧上；
 * bink_frame_seek_pos() 会报告实际落点。解码器参考状态
 * 会被自动刷新。
 */
int bink_seek(BinkFile *f, int64_t frame);

/** 最近一次跳转落到的帧号（从未跳转时为 -1）。 */
int64_t bink_seek_pos(BinkFile *f);

/**
 * 将已解码帧（YUV420P）转换为 RGB24。
 * @param r          已解码帧
 * @param rgb        输出缓冲区，至少 height*width*3 字节
 * @param rgb_stride 每行输出字节数（打包 RGB 时传入 width*3）
 */
void bink_frame_to_rgb(const BinkFrameResult *r, uint8_t *rgb, int rgb_stride);

#ifdef __cplusplus
}
#endif

#endif /* BINK_API_H */
