/*
 * Bink decoder - standalone C port from FFmpeg
 *
 * This file is part of the standalone Bink decoder project (LGPL 2.1+).
 * See bink_common.h for the full license header.
 */

/**
 * @file bink_video.h
 *
 * Bink 视频解码器（Bink 1，版本 'b'..'k'）。这是 FFmpeg 的
 * libavcodec/bink.c + binkdsp.c + blockdsp.c 的无依赖移植版本。
 */

#ifndef BINK_VIDEO_H
#define BINK_VIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BinkVideo BinkVideo;

/**
 * 解码后的视频帧（YUV420P，可选带 alpha 平面）。
 */
typedef struct BinkVideoFrame {
    const uint8_t *planes[4]; /*!< Y、U、V（若 has_alpha 则为 A）       */
    int strides[4];           /*!< 每行每个平面的字节数                */
    int width;                /*!< 亮度宽度                            */
    int height;               /*!< 亮度高度                            */
    int has_alpha;            /*!< planes[3] 有效时非零                 */
    int keyframe;             /*!< 是关键帧时非零                       */
} BinkVideoFrame;

/**
 * 创建一个 Bink 视频解码器。
 *
 * @param out             接收新解码器（失败时为 NULL）
 * @param width,height    视频尺寸
 * @param extradata       编解码器附加数据（容器的 4 字节标志字段），可为 NULL
 * @param extradata_size  附加数据的大小
 * @param codec_tag       例如 "BIKf"/"BIKg"/"BIKh"/"BIKi"/"BIKj"/"BIKk"
 *                        （第一个字节被忽略，第二个字节为版本）
 * @return 成功返回 0，否则返回负的错误码
 */
int bink_video_init(BinkVideo **out, int width, int height,
                    const uint8_t *extradata, int extradata_size,
                    const char *codec_tag);

/**
 * 将一个视频数据包解码到内部帧缓冲区。
 *
 * @param v          解码器
 * @param pkt        数据包负载
 * @param pkt_size   数据包大小（字节）
 * @param keyframe   可选，若解码出的帧是关键帧则接收 1
 * @return 成功返回 0，否则返回负的错误码
 */
int bink_video_decode(BinkVideo *v, const uint8_t *pkt, int pkt_size,
                      int *keyframe);

/**
 * 返回最近一次解码得到的帧。
 */
void bink_video_get_frame(BinkVideo *v, BinkVideoFrame *frame);

/**
 * 便捷函数：将最近一次解码得到的 YUV 帧转换为 RGB24。
 * @param rgb 输出缓冲区，必须至少能容纳 width*height*3 字节
 */
void bink_video_to_rgb(BinkVideo *v, uint8_t *rgb);

/**
 * 释放 Bink 视频解码器及其所有缓冲区。
 */
void bink_video_free(BinkVideo **v);

/**
 * 重置解码器状态（参考帧、帧计数器）。应在 seek 之后调用，
 * 以确保帧间帧不会基于 seek 之前的数据进行预测。
 */
void bink_video_flush(BinkVideo *v);

#ifdef __cplusplus
}
#endif

#endif /* BINK_VIDEO_H */
