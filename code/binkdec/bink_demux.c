/*
 * Bink decoder - standalone C port from FFmpeg
 *
 * Bink demuxer
 * Copyright (c) 2008-2010 Peter Ross (pross@xvid.org)
 * Copyright (c) 2009 Daniel Verkamp (daniel@drv.nu)
 *
 * This file is part of the standalone Bink decoder project (LGPL 2.1+).
 * See bink_common.h for the full license header.
 */

/**
 * @file bink_demux.c
 *
 * FFmpeg 的 libavformat/bink.c 解复用器的无依赖移植。
 *
 * 技术细节：
 *   http://wiki.multimedia.cx/index.php?title=Bink_Container
 */

#include "bink_demux.h"
#include "bink_common.h"

/* 音频轨标志位（每条音频轨的 16 位标志字）。 */
enum BinkAudFlags {
    BINK_AUD_16BITS = 0x4000, /*!< 优先 16 位输出 */
    BINK_AUD_STEREO = 0x2000,
    BINK_AUD_USEDCT = 0x1000,
};

#define BINK_EXTRADATA_SIZE 4

typedef struct BinkFrame {
    uint32_t pos;          /*!< 绝对偏移（含 smush_size） */
    uint32_t size;         /*!< 帧大小（字节）          */
    int keyframe;          /*!< 非零表示关键帧         */
} BinkFrame;

typedef struct BinkAudioTrack {
    int sample_rate;
    int channels;
    int use_dct;
    uint32_t id;
    int64_t pts;
} BinkAudioTrack;

struct BinkDemuxer {
    /* 自有的文件缓冲区，末尾带 BINK_INPUT_BUFFER_PADDING 字节的零填充 */
    uint8_t *data;
    int file_size;              /*!< 文件头中声明的大小   */
    int real_size;              /*!< 缓冲区/数据的实际大小 */

    BinkStreamInfo video;           /* 视频流信息 */

    BinkAudioTrack audio[BINK_MAX_AUDIO_TRACKS];
    int nb_audio;                   /* 音频轨数量 */

    BinkFrame *frames;              /* 帧索引表 */
    int nb_frames;

    int smush_size;                 /* 被 SMUSH 头跳过的字节数 */
    int flags;                      /* 当前帧的关键帧标志       */

    /* 读取状态 */
    int current_track;              /* 下一个要返回的音频轨        */
    int64_t video_pts;              /* 下一个视频包的 PTS          */
    uint32_t remain_packet_size;    /* 当前视频帧剩余的字节数      */
    int64_t seek_frame;             /* 上次 seek 落到的帧          */
};

static int bink_read_frame_index(BinkDemuxer *b, const uint8_t *idx)
{
    const uint8_t *p = idx;
    uint32_t pos, next_pos;
    int next_keyframe = 1, keyframe;
    int i;

    next_pos = bink_rl32(p);
    p += 4;

    for (i = 0; i < b->nb_frames; i++) {
        pos = next_pos;
        keyframe = next_keyframe;
        if (i == b->nb_frames - 1) {
            next_pos = (uint32_t)b->file_size;
            next_keyframe = 0;
        } else {
            next_pos = bink_rl32(p);
            p += 4;
            next_keyframe = (int)(next_pos & 1);
        }
        pos &= ~1u;
        next_pos &= ~1u;

        if (next_pos <= pos) {
            bink_log(BINK_LOG_ERROR, "invalid frame index table\n");
            return BINK_ERROR_EIO;
        }
        /* 索引位置是相对于 BIK 数据的；加上 SMUSH 头大小
           使其变为绝对位置 */
        b->frames[i].pos      = pos + b->smush_size;
        b->frames[i].size     = next_pos - pos;
        b->frames[i].keyframe = keyframe;
    }
    return 0;
}

int bink_demux_open_memory(BinkDemuxer **out, const uint8_t *buf, int size)
{
    BinkDemuxer *b;
    const uint8_t *p;
    uint32_t fps_num, fps_den;
    uint8_t revision;
    uint32_t largest_frame;
    unsigned int i;
    int ret;

    if (!out || !buf || size < 40)
        return BINK_ERROR_EINVAL;

    b = (BinkDemuxer *)bink_calloc(1, sizeof(BinkDemuxer));
    if (!b)
        return BINK_ERROR_ENOMEM;

    /* 复制文件并附加解码填充，以便位读取器可以越界读取 */
    b->data = (uint8_t *)bink_malloc((size_t)size + BINK_INPUT_BUFFER_PADDING);
    if (!b->data) {
        bink_free(b);
        return BINK_ERROR_ENOMEM;
    }
    memcpy(b->data, buf, (size_t)size);
    memset(b->data + size, 0, BINK_INPUT_BUFFER_PADDING);
    b->file_size = size;

    p = b->data;

    b->video.codec_tag = bink_rl32(p);
    p += 4;

    if (b->video.codec_tag == MKTAG('S', 'M', 'U', 'S')) {
        /* 传统 SMUSH 包装格式：向前搜索 BIK 签名 */
        do {
            b->smush_size += BINK_SMUSH_BLOCK_SIZE;
            p += BINK_SMUSH_BLOCK_SIZE - 4;
            if (p + 4 > b->data + b->file_size)
                goto smush_fail;
            b->video.codec_tag = bink_rl32(p);
            p += 4;
        } while ((b->video.codec_tag & 0xFFFFFF) != AV_RL32_SIG('B', 'I', 'K', 0));
    }

    /* 此解码器不支持 Bink2 (KB2)：提前拒绝，以免 .bk2 文件
       被当作 Bink1 容器半解析。 */
    if ((b->video.codec_tag & 0xFFFFFF) == AV_RL32_SIG('K', 'B', '2', 0)) {
        bink_log(BINK_LOG_ERROR, "Bink2 (KB2) files are not supported\n");
        ret = BINK_ERROR_NOTSUPPORTED;
        goto fail;
    }

    if (p + 40 > b->data + b->file_size)
        goto truncated;

    b->file_size = (int)(bink_rl32(p) + 8);
    b->real_size = size;
    p += 4;
    b->video.nb_frames = (int)bink_rl32(p);
    p += 4;

    if (b->video.nb_frames > 1000000) {
        bink_log(BINK_LOG_ERROR, "invalid header: more than 1000000 frames\n");
        ret = BINK_ERROR_EIO;
        goto fail;
    }

    largest_frame = bink_rl32(p);
    p += 4;
    if (largest_frame > (uint32_t)b->file_size) {
        bink_log(BINK_LOG_ERROR,
                 "invalid header: largest frame size greater than file size\n");
        ret = BINK_ERROR_EIO;
        goto fail;
    }

    p += 4; /* 保留 / 采样率字段 */

    b->video.width  = (int)bink_rl32(p);
    p += 4;
    b->video.height = (int)bink_rl32(p);
    p += 4;

    fps_num = bink_rl32(p);
    p += 4;
    fps_den = bink_rl32(p);
    p += 4;
    if (fps_num == 0 || fps_den == 0) {
        bink_log(BINK_LOG_ERROR, "invalid header: invalid fps (%u/%u)\n",
                 fps_num, fps_den);
        ret = BINK_ERROR_EIO;
        goto fail;
    }
    b->video.fps_num = (int)fps_num;
    b->video.fps_den = (int)fps_den;
    b->video.type    = BINK_STREAM_VIDEO;

    if (p + 4 > b->data + b->file_size)
        goto truncated;
    memcpy(b->video.video_flags, p, 4); /* 4 字节视频额外数据 */
    p += 4;

    b->nb_audio = (int)bink_rl32(p);
    p += 4;

    if (b->nb_audio > BINK_MAX_AUDIO_TRACKS) {
        bink_log(BINK_LOG_ERROR,
                 "invalid header: more than %d audio tracks (%d)\n",
                 BINK_MAX_AUDIO_TRACKS, b->nb_audio);
        ret = BINK_ERROR_EIO;
        goto fail;
    }

    revision = (uint8_t)((b->video.codec_tag >> 24) & 0xFF);

    if ((b->video.codec_tag & 0xFFFFFF) == AV_RL32_SIG('B', 'I', 'K', 0) &&
        revision == 'k')
        p += 4; /* 未知的新字段 */

    if (b->nb_audio) {
        if (p + (size_t)4 * b->nb_audio > b->data + b->file_size)
            goto truncated;
        p += 4 * (size_t)b->nb_audio; /* 每条音频轨的最大解码大小 */

        for (i = 0; i < (unsigned int)b->nb_audio; i++) {
            uint16_t flags;
            if (p + 4 > b->data + b->file_size)
                goto truncated;
            b->audio[i].sample_rate = (int)bink_rl16(p);
            p += 2;
            flags = bink_rl16(p);
            p += 2;
            b->audio[i].use_dct  = (flags & BINK_AUD_USEDCT) ? 1 : 0;
            b->audio[i].channels = (flags & BINK_AUD_STEREO) ? 2 : 1;
            b->audio[i].pts      = 0;
        }

        for (i = 0; i < (unsigned int)b->nb_audio; i++) {
            if (p + 4 > b->data + b->file_size)
                goto truncated;
            b->audio[i].id = bink_rl32(p);
            p += 4;
        }
    }

    /* 帧索引表 */
    b->nb_frames = b->video.nb_frames;
    b->frames = (BinkFrame *)bink_calloc((size_t)b->nb_frames,
                                         sizeof(BinkFrame));
    if (!b->frames && b->video.nb_frames > 0) {
        ret = BINK_ERROR_ENOMEM;
        goto fail;
    }
    if ((ret = bink_read_frame_index(b, p)) < 0)
        goto fail;

    b->current_track = -1;
    b->seek_frame = -1;

    *out = b;
    return 0;

truncated:
    bink_log(BINK_LOG_ERROR, "truncated Bink header\n");
    ret = BINK_ERROR_INVALIDDATA;
    goto fail;
smush_fail:
    bink_log(BINK_LOG_ERROR, "invalid SMUSH header: BIK not found\n");
    ret = BINK_ERROR_INVALIDDATA;
    goto fail;
fail:
    bink_demux_close(&b);
    return ret;
}

int bink_demux_open_file(BinkDemuxer **out, const char *path)
{
    FILE *f;
    long sz;
    uint8_t *buf;
    int ret;

    if (!out || !path)
        return BINK_ERROR_EINVAL;

    f = fopen(path, "rb");
    if (!f)
        return BINK_ERROR_EIO;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return BINK_ERROR_EIO;
    }
    sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return BINK_ERROR_EIO;
    }

    buf = (uint8_t *)bink_malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return BINK_ERROR_ENOMEM;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        bink_free(buf);
        fclose(f);
        return BINK_ERROR_EIO;
    }
    fclose(f);

    ret = bink_demux_open_memory(out, buf, (int)sz);
    bink_free(buf);
    return ret;
}

int bink_demux_get_stream_count(BinkDemuxer *d)
{
    if (!d)
        return 0;
    return 1 + d->nb_audio;
}

const BinkStreamInfo *bink_demux_get_stream(BinkDemuxer *d, int index)
{
    static BinkStreamInfo audio_info; /* 静态，保证指针一直有效 */
    if (!d)
        return NULL;
    if (index == 0)
        return &d->video;
    if (index < 1 + d->nb_audio) {
        audio_info.type        = BINK_STREAM_AUDIO;
        audio_info.sample_rate = d->audio[index - 1].sample_rate;
        audio_info.channels    = d->audio[index - 1].channels;
        audio_info.use_dct     = d->audio[index - 1].use_dct;
        return &audio_info;
    }
    return NULL;
}

int bink_demux_read_packet(BinkDemuxer *d, BinkPacket *pkt)
{
    const uint8_t *p;

    if (!d || !pkt)
        return BINK_ERROR_EINVAL;

    if (d->current_track < 0) {
        if (d->video_pts >= d->video.nb_frames)
            return 0; /* 文件末尾 */
        d->remain_packet_size = d->frames[d->video_pts].size;
        d->flags              = d->frames[d->video_pts].keyframe;
        d->current_track      = 0;
    }

    /* 文件被截断：偏移落在数据真实末尾之后的帧无法解码，
       应干净地停止而不是越界读取 */
    if (d->frames[d->video_pts].pos >= (uint32_t)d->real_size)
        return 0;

    while (d->current_track < d->nb_audio) {
        uint32_t audio_size;
        int track = d->current_track;

        p = d->data + d->frames[d->video_pts].pos;
        if (d->remain_packet_size < 4)
            return BINK_ERROR_EIO;
        audio_size = bink_rl32(p);

        if (audio_size > d->remain_packet_size - 4) {
            bink_log(BINK_LOG_ERROR,
                     "frame %lld: audio size in header (%u) > size of "
                     "packet left (%u)\n",
                     (long long)d->video_pts, audio_size,
                     d->remain_packet_size);
            return BINK_ERROR_EIO;
        }
        /* 文件被截断：音频块超出了真实数据范围 */
        if ((uint32_t)(p - d->data) + 4 + audio_size > (uint32_t)d->real_size)
            return 0;

        d->remain_packet_size -= 4 + audio_size;
        d->current_track++;

        if (audio_size >= 4) {
            const uint8_t *ap = p + 4;

            pkt->stream_index = 1 + track;
            pkt->pts          = d->audio[track].pts;
            pkt->flags        = 0;
            pkt->data         = ap;
            pkt->size         = (int)audio_size;

            /* 每个音频包报告解压缩样本数（字节）；用它来
               推进音频 PTS */
            d->audio[track].pts += (int64_t)bink_rl32(ap) /
                                   (2 * d->audio[track].channels);
            return 1;
        }
        /* audio_size < 4：跳过（填充的）音频字节 */
    }

    /* 视频包：帧以一个 4 字节大小头 + 各音频轨的音频数据开头；
       视频比特流紧随其后。跳过音频部分（frames[size] -
       remain_packet_size 给出音频头和数据消耗的字节数）。 */
    pkt->stream_index = 0;
    pkt->pts          = d->video_pts++;
    pkt->flags        = d->flags ? BINK_PKT_FLAG_KEY : 0;
    {
        uint32_t vpos = d->frames[pkt->pts].pos +
                        (d->frames[pkt->pts].size - d->remain_packet_size);
        /* 文件被截断：一旦帧会超出数据真实末尾，就干净地停止
           （其比特流反正也是不完整的） */
        if (vpos >= (uint32_t)d->real_size ||
            vpos + d->remain_packet_size > (uint32_t)d->real_size)
            return 0;
        pkt->data = d->data + vpos;
        pkt->size = (int)d->remain_packet_size;
    }

    /* -1 指示下一次调用读取下一帧 */
    d->current_track = -1;

    return 1;
}

int bink_demux_seek(BinkDemuxer *d, int64_t frame)
{
    int64_t target = frame;

    if (!d)
        return BINK_ERROR_EINVAL;
    if (d->video.nb_frames <= 0)
        return 0;

    if (target < 0)
        target = 0;
    if (target >= d->video.nb_frames)
        target = d->video.nb_frames - 1;

    /* 回退到最近的关键帧（中间帧依赖先前解码的帧） */
    while (target > 0 && !d->frames[target].keyframe)
        target--;

    d->video_pts        = target;
    d->current_track    = -1;
    d->seek_frame       = target;
    {
        int i;
        for (i = 0; i < d->nb_audio; i++)
            d->audio[i].pts = 0;
    }
    return 0;
}

int64_t bink_demux_get_seek_frame(BinkDemuxer *d)
{
    return d ? d->seek_frame : -1;
}

void bink_demux_close(BinkDemuxer **d)
{
    if (!d || !*d)
        return;
    bink_free((*d)->frames);
    bink_free((*d)->data);
    bink_free(*d);
    *d = NULL;
}
