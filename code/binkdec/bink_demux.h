/*
 * Bink decoder - standalone C port from FFmpeg
 *
 * This file is part of the standalone Bink decoder project (LGPL 2.1+).
 * See bink_common.h for the full license header.
 */

/**
 * @file bink_demux.h
 *
 * Bink (.bik) 容器解析器。
 *
 * 这是 FFmpeg 的 libavformat/bink.c 解复用器
 * （Bink / SMUSH 容器解复用器）的无依赖移植。它基于整个文件的
 * 内存缓冲区工作，因此不需要文件 I/O 或带缓冲的 I/O 层。
 */

#ifndef BINK_DEMUX_H
#define BINK_DEMUX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BINK_MAX_AUDIO_TRACKS 256
#define BINK_MAX_WIDTH        7680
#define BINK_MAX_HEIGHT       4800
#define BINK_SMUSH_BLOCK_SIZE 512

/* 流类型 */
#define BINK_STREAM_VIDEO 0
#define BINK_STREAM_AUDIO 1

/* 包标志 */
#define BINK_PKT_FLAG_KEY (1 << 0)   /*!< 视频包是关键帧 */

/**
 * 从容器头中提取的每流信息。
 */
typedef struct BinkStreamInfo {
    int type;               /*!< BINK_STREAM_VIDEO 或 BINK_STREAM_AUDIO */

    /* 视频字段（当 type == BINK_STREAM_VIDEO 时有效） */
    uint32_t codec_tag;     /*!< 例如 "BIKf" / "BIKg"（小端）       */
    int width;              /*!< 亮度宽度                             */
    int height;             /*!< 亮度高度                             */
    int fps_num;            /*!< 帧率分子                             */
    int fps_den;            /*!< 帧率分母                             */
    int nb_frames;          /*!< 视频帧数量                           */
    uint8_t video_flags[4]; /*!< 4 字节视频额外数据（标志）           */

    /* 音频字段（当 type == BINK_STREAM_AUDIO 时有效） */
    int sample_rate;        /*!< 音频采样率                       */
    int channels;           /*!< 1 或 2                           */
    int use_dct;            /*!< 非零表示 DCT，零表示 RDFT       */
} BinkStreamInfo;

/**
 * 一个已解复用的包。data 指向由解复用器拥有的缓冲区；
 * 在 bink_demux_close() 之前一直有效。
 */
typedef struct BinkPacket {
    int stream_index;       /*!< 0 = 视频，1..N = 音频轨索引    */
    int64_t pts;            /*!< 显示时间戳                       */
    int flags;              /*!< BINK_PKT_FLAG_*                 */
    const uint8_t *data;    /*!< 包负载                           */
    int size;               /*!< 包大小（字节）                   */
} BinkPacket;

typedef struct BinkDemuxer BinkDemuxer;

/**
 * 从内存缓冲区打开 Bink / SMUSH 容器。
 * 缓冲区会被复制（并附加解码填充），因此调用方在此调用返回后
 * 可以释放它。
 *
 * @param out   接收解复用器（失败时为 NULL）
 * @param buf   整个文件内容
 * @param size  文件大小（字节）
 * @return 成功返回 0，否则返回负错误码
 */
int bink_demux_open_memory(BinkDemuxer **out, const uint8_t *buf, int size);

/**
 * 从磁盘文件打开 Bink 容器。
 *
 * @param out  接收解复用器（失败时为 NULL）
 * @param path .bik 文件的路径
 * @return 成功返回 0，否则返回负错误码
 */
int bink_demux_open_file(BinkDemuxer **out, const char *path);

/** 流的数量（1 个视频流 + 音频轨）。 */
int bink_demux_get_stream_count(BinkDemuxer *d);

/**
 * 只读的流信息。
 * @param index 0 = 视频流，1..N-1 = 音频轨
 */
const BinkStreamInfo *bink_demux_get_stream(BinkDemuxer *d, int index);

/**
 * 读取下一个包。一个视频帧的音频包会先返回
 * （每个音频轨一个），然后返回该帧的视频包。
 *
 * @param d   解复用器
 * @param pkt 接收包
 * @return 成功返回 1，文件末尾返回 0，否则返回负错误码
 */
int bink_demux_read_packet(BinkDemuxer *d, BinkPacket *pkt);

/**
 * 跳转到视频帧。由于 Bink 中间帧依赖前一帧，
 * 实际 seek 会落在请求帧之前或该帧处最近的关键帧上；
 * bink_demux_get_seek_frame() 报告实际落点。
 *
 * @param d     解复用器
 * @param frame 从零开始的视频帧号
 * @return 成功返回 0，否则返回负错误码
 */
int bink_demux_seek(BinkDemuxer *d, int64_t frame);

/** 上次 seek 落到的帧号（从未 seek 过则为 -1）。 */
int64_t bink_demux_get_seek_frame(BinkDemuxer *d);

/** 释放解复用器及其所有缓冲区。 */
void bink_demux_close(BinkDemuxer **d);

#ifdef __cplusplus
}
#endif

#endif /* BINK_DEMUX_H */
