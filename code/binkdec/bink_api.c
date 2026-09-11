/*
 * Bink decoder - standalone C port from FFmpeg
 *
 * This file is part of the standalone Bink decoder project (LGPL 2.1+).
 * See bink_common.h for the full license header.
 */

/**
 * @file bink_api.c
 *
 * 高层 C API：解封装器 + 视频解码器 + 音频解码器的编排。
 */

#include "bink_api.h"
#include "bink_common.h"
#include "bink_video.h"
#include "bink_audio.h"

#define BINK_API_SCRATCH_FLOATS 4096 /* 最大 frame_len（RDFT 立体声） */

struct BinkFile {
    BinkDemuxer *demux;
    BinkVideo *video;
    BinkAudio *audio[BINK_MAX_AUDIO_TRACKS];
    int nb_audio;

    BinkStreamInfo streams[BINK_MAX_AUDIO_TRACKS + 1];
    int nb_streams;

    /* 当前帧各音轨交错 PCM 的累积缓冲 */
    float *audio_buf[BINK_MAX_AUDIO_TRACKS];
    int audio_cap[BINK_MAX_AUDIO_TRACKS];
    int audio_len[BINK_MAX_AUDIO_TRACKS];

    /* 一个已解码音频块（每声道）的共享临时缓冲区 */
    float *scratch[BINK_AUDIO_MAX_CHANNELS];

    int eof;
};

static int bink_api_create_audio(BinkFile *f)
{
    int i;

    for (i = 0; i < f->nb_audio; i++) {
        const BinkStreamInfo *s = &f->streams[i + 1];
        uint8_t tag[4];
        int ret;

        AV_WL32(tag, f->streams[0].codec_tag);
        ret = bink_audio_init(&f->audio[i], s->sample_rate, s->channels,
                              s->use_dct, tag, 4);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int bink_open_memory(BinkFile **out, const uint8_t *buf, int size)
{
    BinkFile *f;
    int i, ret;

    if (!out)
        return BINK_ERROR_EINVAL;
    *out = NULL;

    f = (BinkFile *)bink_calloc(1, sizeof(BinkFile));
    if (!f)
        return BINK_ERROR_ENOMEM;

    if ((ret = bink_demux_open_memory(&f->demux, buf, size)) < 0)
        goto fail;

    f->nb_streams = bink_demux_get_stream_count(f->demux);
    f->nb_audio   = f->nb_streams - 1;
    for (i = 0; i < f->nb_streams; i++) {
        const BinkStreamInfo *si = bink_demux_get_stream(f->demux, i);
        if (si)
            f->streams[i] = *si;
    }

    /* 视频解码器 */
    if (f->streams[0].type == BINK_STREAM_VIDEO) {
        uint8_t tag[4];
        AV_WL32(tag, f->streams[0].codec_tag);
        ret = bink_video_init(&f->video,
                              f->streams[0].width, f->streams[0].height,
                              f->streams[0].video_flags, 4,
                              (const char *)tag);
        if (ret < 0)
            goto fail;
    }

    if ((ret = bink_api_create_audio(f)) < 0)
        goto fail;

    for (i = 0; i < BINK_AUDIO_MAX_CHANNELS; i++) {
        f->scratch[i] = (float *)bink_malloc(
            BINK_API_SCRATCH_FLOATS * sizeof(float));
        if (!f->scratch[i]) {
            ret = BINK_ERROR_ENOMEM;
            goto fail;
        }
    }

    *out = f;
    return 0;

fail:
    bink_close(&f);
    return ret;
}

int bink_open(BinkFile **out, const char *path)
{
    uint8_t *buf;
    long sz;
    FILE *fl;
    int ret;

    if (!out || !path)
        return BINK_ERROR_EINVAL;
    *out = NULL;

    fl = fopen(path, "rb");
    if (!fl)
        return BINK_ERROR_EIO;
    if (fseek(fl, 0, SEEK_END) != 0) {
        fclose(fl);
        return BINK_ERROR_EIO;
    }
    sz = ftell(fl);
    if (sz < 0 || fseek(fl, 0, SEEK_SET) != 0) {
        fclose(fl);
        return BINK_ERROR_EIO;
    }
    buf = (uint8_t *)bink_malloc((size_t)sz);
    if (!buf) {
        fclose(fl);
        return BINK_ERROR_ENOMEM;
    }
    if (fread(buf, 1, (size_t)sz, fl) != (size_t)sz) {
        bink_free(buf);
        fclose(fl);
        return BINK_ERROR_EIO;
    }
    fclose(fl);

    ret = bink_open_memory(out, buf, (int)sz);
    bink_free(buf);
    return ret;
}

void bink_close(BinkFile **pf)
{
    BinkFile *f;
    int i;

    if (!pf || !*pf)
        return;
    f = *pf;

    bink_video_free(&f->video);
    for (i = 0; i < f->nb_audio; i++)
        bink_audio_free(&f->audio[i]);
    for (i = 0; i < BINK_AUDIO_MAX_CHANNELS; i++)
        bink_free(f->scratch[i]);
    for (i = 0; i < BINK_MAX_AUDIO_TRACKS; i++)
        bink_free(f->audio_buf[i]);
    bink_demux_close(&f->demux);
    bink_free(f);
    *pf = NULL;
}

int bink_stream_count(BinkFile *f)
{
    return f ? f->nb_streams : 0;
}

const BinkStreamInfo *bink_stream(BinkFile *f, int index)
{
    if (!f || index < 0 || index >= f->nb_streams)
        return NULL;
    return &f->streams[index];
}

int bink_has_video_decoder(BinkFile *f)
{
    return f ? (f->video != NULL) : 0;
}

static int bink_append_audio(BinkFile *f, int track, int n)
{
    const BinkStreamInfo *s = &f->streams[track + 1];
    int chans = s->channels;
    int need = f->audio_len[track] + n * chans;
    int c, i;

    if (need > f->audio_cap[track]) {
        int newcap = f->audio_cap[track] ? f->audio_cap[track] * 2 : 4096;
        float *nb;
        while (newcap < need)
            newcap *= 2;
        nb = (float *)bink_realloc(f->audio_buf[track],
                                   (size_t)newcap * sizeof(float));
        if (!nb)
            return BINK_ERROR_ENOMEM;
        f->audio_buf[track] = nb;
        f->audio_cap[track] = newcap;
    }

    /* scratch[c] 始终保存声道 c 的 `n` 个样本（DCT 与 RDFT 解码器
       均输出按声道划分的缓冲区）；在此处将它们交错排列。
       audio_len 统计交错后的样本数，因此声道 `c` 的第 `i` 个帧样本
       位于 audio_len + i*chans + c。 */
    for (c = 0; c < chans; c++)
        for (i = 0; i < n; i++)
            f->audio_buf[track][f->audio_len[track] + i * chans + c] =
                f->scratch[c][i];
    f->audio_len[track] += n * chans;
    return 0;
}

static int bink_decode_audio_packet(BinkFile *f, int track,
                                    const uint8_t *pkt, int pkt_size)
{
    BinkAudio *a = f->audio[track];
    int fed = 0;
    int ret;

    while (1) {
        int n = 0;
        ret = bink_audio_decode(a, fed ? NULL : pkt, fed ? 0 : pkt_size,
                                f->scratch, &n);
        if (ret <= 0)
            break;
        fed = 1;
        if ((ret = bink_append_audio(f, track, n)) < 0)
            return ret;
    }
    if (ret < 0)
        return ret;
    return 0;
}

int bink_decode_frame(BinkFile *f, BinkFrameResult *r)
{
    BinkPacket pkt;
    BinkVideoFrame vf;
    int i, ret;

    if (!f || !r)
        return BINK_ERROR_EINVAL;

    if (f->eof)
        return 0;

    memset(r, 0, sizeof(*r));
    for (i = 0; i < f->nb_audio; i++)
        f->audio_len[i] = 0;

    while (1) {
        ret = bink_demux_read_packet(f->demux, &pkt);
        if (ret <= 0) {
            if (ret == 0)
                f->eof = 1;
            return ret;
        }

        if (pkt.stream_index >= 1) {
            int track = pkt.stream_index - 1;
            if (track < f->nb_audio && f->audio[track])
                bink_decode_audio_packet(f, track, pkt.data, pkt.size);
            continue;
        }

        /* 视频数据包 */
        r->pts = pkt.pts;
        r->keyframe = (pkt.flags & BINK_PKT_FLAG_KEY) ? 1 : 0;
        r->width  = f->streams[0].width;
        r->height = f->streams[0].height;

        if (f->video) {
            int kf = 0;
            if ((ret = bink_video_decode(f->video, pkt.data, pkt.size, &kf)) < 0)
                return ret;
            bink_video_get_frame(f->video, &vf);
            r->keyframe = kf;
            r->width    = vf.width;
            r->height   = vf.height;
            r->has_alpha = vf.has_alpha;
            for (i = 0; i < 4; i++) {
                r->planes[i] = vf.planes[i];
                r->strides[i] = vf.strides[i];
            }
        }

        /* 音频结果 */
        r->nb_audio = 0;
        for (i = 0; i < f->nb_audio; i++) {
            const BinkStreamInfo *s = &f->streams[i + 1];
            r->audio_channels[i]     = s->channels;
            r->audio_sample_rate[i]  = s->sample_rate;
            if (f->audio_len[i] > 0) {
                r->audio_nb_samples[i] = f->audio_len[i] / s->channels;
                r->audio[i]            = f->audio_buf[i];
            } else {
                r->audio_nb_samples[i] = 0;
                r->audio[i]            = NULL;
            }
            r->nb_audio++;
        }
        return 1;
    }
}

int bink_seek(BinkFile *f, int64_t frame)
{
    int i, ret;

    if (!f)
        return BINK_ERROR_EINVAL;

    ret = bink_demux_seek(f->demux, frame);
    if (ret < 0)
        return ret;

    bink_video_flush(f->video);
    for (i = 0; i < f->nb_audio; i++) {
        bink_audio_flush(f->audio[i]);
        f->audio_len[i] = 0;
    }
    f->eof = 0;
    return 0;
}

int64_t bink_seek_pos(BinkFile *f)
{
    return f ? bink_demux_get_seek_frame(f->demux) : -1;
}

void bink_frame_to_rgb(const BinkFrameResult *r, uint8_t *rgb, int rgb_stride)
{
    const uint8_t *y, *u, *v;
    int ys, us, vs;
    int x, yy;

    if (!r || !rgb || !r->planes[0])
        return;

    y  = r->planes[0];  ys = r->strides[0];
    u  = r->planes[1];  us = r->strides[1];
    v  = r->planes[2];  vs = r->strides[2];

    for (yy = 0; yy < r->height; yy++) {
        uint8_t *dst = rgb + (size_t)yy * rgb_stride;
        const uint8_t *yl = y + (size_t)yy * ys;
        const uint8_t *ul = u + (size_t)(yy >> 1) * us;
        const uint8_t *vl = v + (size_t)(yy >> 1) * vs;
        for (x = 0; x < r->width; x++) {
            int yyv = yl[x] - 16;
            int cb  = ul[x >> 1] - 128;
            int cr  = vl[x >> 1] - 128;
            int rr, gg, bb;

            rr = (298 * yyv + 409 * cr + 128) >> 8;
            gg = (298 * yyv - 100 * cb - 208 * cr + 128) >> 8;
            bb = (298 * yyv + 516 * cb + 128) >> 8;

            dst[0] = (uint8_t)bink_clip(rr, 0, 255);
            dst[1] = (uint8_t)bink_clip(gg, 0, 255);
            dst[2] = (uint8_t)bink_clip(bb, 0, 255);
            dst += 3;
        }
    }
}
