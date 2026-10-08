/*
 * Allwinner Cedar (VE) hardware H.264 decoder through libcedarc, with a software fallback.
 *
 * FFmpeg 5.1 port of mpv-tsp's cedar/cedardec.c, for GStreamer's gst-libav in the WPE browser:
 * - decoded pictures (NV21, in ION memory) are copied into regular frames, so GStreamer sees an
 *   ordinary decoder (no AV_CODEC_CAP_HARDWARE: gst-libav skips hardware decoders);
 * - when the hardware can't be used (no /dev/cedar_dev, it fails to initialize, High 10/4:2:2/
 *   4:4:4 profiles), FFmpeg's own h264 decoder runs inside this one instead, so playback works
 *   on any device: GStreamer has no fallback to another decoder once one is chosen;
 * - gst-libav matches output frames to its input through reordered_opaque (the input frame
 *   number), which each output frame must carry from its own packet. It also expects the frame
 *   buffer to be requested when decoding starts, in decoding order, as FFmpeg's decoders do: a
 *   frame still without one when a later one comes out is dropped as a "ghost frame" (with
 *   B-frames, a P-frame decoded early comes out late). So a buffer is reserved per packet as it
 *   goes into the decoder, and the picture is copied into it when it comes out.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <string.h>

#include <vdecoder.h>
#include <memoryAdapter.h>

#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/thread.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "internal.h"

/* Not in libcedarc's public headers (AddVDPlugin is declared in vdecoder.h) */
void log_set_level(unsigned level);
#define CEDAR_LOG_LEVEL_ERROR 6

#define MAX_DECODE_STEPS 64 /* DecodeVideoStream() calls per receive_frame() */
#define MAX_PENDING 12      /* packets in flight in the decoder (each holds a reserved frame) */
#define MAX_RESERVED 24     /* reserved frames (software fallback: its delay + frame threads) */

/* Timestamps are not given to the decoder: with them it drops frames it considers late and
 * treats negative ones (the pre-roll of an MP4 edit list) as missing. Pictures come out in
 * display order, so each one gets the smallest pending input timestamp, together with that
 * packet's discard flag (edit-list pre-roll) and reordered_opaque. */
typedef struct CedarPktInfo {
    int64_t pts;
    int64_t reordered_opaque;
    int discard;
} CedarPktInfo;

typedef struct Reserved {
    int64_t reordered_opaque;
    AVFrame *frame;
} Reserved;

typedef struct CedarDecContext {
    AVClass *class;
    VideoDecoder *dec;
    struct ScMemOpsS *memops;
    AVPacket *pkt;          /* packet waiting for room in the stream buffer */
    int pkt_pending;
    int eof;                /* all input was submitted */
    CedarPktInfo pending[MAX_PENDING]; /* input timestamps not yet given to a picture */
    int nb_pending;
    int64_t nb_submitted, nb_pictures, nb_discarded, nb_no_pts;
    /* software fallback: FFmpeg's h264 decoder, its frames copied into ours */
    AVCodecContext *sw;
    AVFrame *sw_frame;
    int sw_drain_sent;
    Reserved reserved[MAX_RESERVED]; /* frame buffers requested in decoding order, oldest first */
    int nb_reserved;
} CedarDecContext;

static AVOnce plugins_once = AV_ONCE_INIT;

static void load_plugins(void)
{
    log_set_level(CEDAR_LOG_LEVEL_ERROR);
    AddVDPlugin();          /* the codec plugins next to libvideoengine.so */
    log_set_level(CEDAR_LOG_LEVEL_ERROR);
}

/* High 10/4:2:2/4:4:4 (profile_idc 110/122/244/44) are beyond the hardware */
static int profile_supported(AVCodecContext *avctx)
{
    const uint8_t *p = avctx->extradata;
    int size = avctx->extradata_size, profile = -1;
    if (size >= 2 && p[0] == 1) {                       /* avcC */
        profile = p[1];
    } else {                                            /* Annex B: find the SPS */
        for (int i = 0; i + 4 < size; i++)
            if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1f) == 7) {
                profile = p[i + 4];
                break;
            }
    }
    if (profile < 0 && avctx->profile > 0)
        profile = avctx->profile & 0xff;
    return profile < 0 || profile <= 100;
}

static void cedar_close_hw(CedarDecContext *s)
{
    if (s->dec)
        DestroyVideoDecoder(s->dec);
    s->dec = NULL;
    if (s->memops)
        CdcMemClose(s->memops);
    s->memops = NULL;
}

static void clear_reserved(CedarDecContext *s)
{
    for (int i = 0; i < s->nb_reserved; i++)
        av_frame_free(&s->reserved[i].frame);
    s->nb_reserved = 0;
}

static av_cold int cedar_close(AVCodecContext *avctx)
{
    CedarDecContext *s = avctx->priv_data;
    clear_reserved(s);
    if (s->sw)
        av_log(avctx, AV_LOG_VERBOSE, "software fallback\n");
    else
        av_log(avctx, AV_LOG_VERBOSE, "%"PRId64" packets, %"PRId64" pictures (%"PRId64" discarded, %"PRId64" without pts)\n",
               s->nb_submitted, s->nb_pictures, s->nb_discarded, s->nb_no_pts);
    cedar_close_hw(s);
    avcodec_free_context(&s->sw);
    av_frame_free(&s->sw_frame);
    av_packet_free(&s->pkt);
    return 0;
}

static int cedar_init_hw(AVCodecContext *avctx)
{
    CedarDecContext *s = avctx->priv_data;
    VideoStreamInfo info;
    VConfig conf;

    if (!profile_supported(avctx)) {
        av_log(avctx, AV_LOG_VERBOSE, "H.264 profile not supported by the hardware\n");
        return AVERROR(ENOSYS);
    }
    ff_thread_once(&plugins_once, load_plugins);

    s->memops = MemAdapterGetOpsS();
    if (!s->memops || CdcMemOpen(s->memops) < 0) {
        s->memops = NULL;
        av_log(avctx, AV_LOG_VERBOSE, "Can't open the ION memory adapter\n");
        return AVERROR_EXTERNAL;
    }
    s->dec = CreateVideoDecoder();
    if (!s->dec) {
        av_log(avctx, AV_LOG_VERBOSE, "CreateVideoDecoder failed\n");
        return AVERROR_EXTERNAL;
    }

    memset(&info, 0, sizeof(info));
    info.eCodecFormat = VIDEO_CODEC_FORMAT_H264;
    info.nWidth = avctx->width;
    info.nHeight = avctx->height;
    info.bIsFramePackage = 1;           /* one access unit per submitted packet */

    memset(&conf, 0, sizeof(conf));
    conf.eOutputPixelFormat = PIXEL_FORMAT_NV21;
    conf.nDeInterlaceHoldingFrameBufferNum = 2;
    conf.nDisplayHoldingFrameBufferNum = 2;
    conf.nRotateHoldingFrameBufferNum = 0;
    conf.nDecodeSmoothFrameBufferNum = 3;
    conf.nAlignStride = 32;             /* the VE writes 32-aligned rows (GPU_ALIGN_STRIDE) */
    conf.memops = s->memops;
    if (InitializeVideoDecoder(s->dec, &info, &conf)) {
        av_log(avctx, AV_LOG_VERBOSE, "InitializeVideoDecoder failed\n");
        return AVERROR_EXTERNAL;
    }
    avctx->pix_fmt = AV_PIX_FMT_NV21;
    return 0;
}

/* FFmpeg's h264 decoder in a context of its own. Its packets are Annex B already (this codec's
 * h264_mp4toannexb bitstream filter, which also repeats SPS/PPS before keyframes), so avcC
 * extradata isn't passed on: it would make the decoder expect length-prefixed NAL units. */
static int cedar_init_sw(AVCodecContext *avctx)
{
    CedarDecContext *s = avctx->priv_data;
    const AVCodec *codec = avcodec_find_decoder_by_name("h264");
    int ret;

    if (!codec)
        return AVERROR_DECODER_NOT_FOUND;
    s->sw = avcodec_alloc_context3(codec);
    s->sw_frame = av_frame_alloc();
    if (!s->sw || !s->sw_frame)
        return AVERROR(ENOMEM);
    s->sw->width = avctx->width;
    s->sw->height = avctx->height;
    /* all cores: this codec has no threading of its own, so FFmpeg set avctx->thread_count to 1 */
    s->sw->thread_count = 0;
    s->sw->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    s->sw->flags = avctx->flags;
    s->sw->flags2 = avctx->flags2;
    s->sw->err_recognition = avctx->err_recognition;
    s->sw->pkt_timebase = avctx->pkt_timebase;
    s->sw->time_base = avctx->time_base;
    if (avctx->extradata_size > 0 && avctx->extradata[0] != 1) {
        s->sw->extradata = av_mallocz(avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!s->sw->extradata)
            return AVERROR(ENOMEM);
        memcpy(s->sw->extradata, avctx->extradata, avctx->extradata_size);
        s->sw->extradata_size = avctx->extradata_size;
    }
    ret = avcodec_open2(s->sw, codec, NULL);
    if (ret < 0)
        return ret;
    avctx->pix_fmt = AV_PIX_FMT_YUV420P; /* until the first frame tells */
    return 0;
}

static av_cold int cedar_init(AVCodecContext *avctx)
{
    CedarDecContext *s = avctx->priv_data;
    int ret;

    s->pkt = av_packet_alloc();
    if (!s->pkt)
        return AVERROR(ENOMEM);
    ret = cedar_init_hw(avctx);
    if (ret >= 0) {
        av_log(avctx, AV_LOG_INFO, "Allwinner Cedar hardware decoder\n");
        return 0;
    }
    cedar_close_hw(s);
    av_log(avctx, AV_LOG_INFO, "Cedar hardware decoder not usable, decoding in software\n");
    return cedar_init_sw(avctx);
}

/* ff_get_buffer() stamps the frame with avctx->reordered_opaque (the last input packet's), and
 * gst-libav's get_buffer2 looks the frame up by it: give it this frame's own value instead. */
static int get_buffer_for(AVCodecContext *avctx, AVFrame *frame, int64_t reordered_opaque)
{
    int64_t saved = avctx->reordered_opaque;
    int ret;
    avctx->reordered_opaque = reordered_opaque;
    ret = ff_get_buffer(avctx, frame, 0);
    avctx->reordered_opaque = saved;
    frame->reordered_opaque = reordered_opaque;
    return ret;
}

/* Request the frame buffer for the packet going into the decoder now (see the top comment) */
static void reserve_frame(AVCodecContext *avctx, enum AVPixelFormat format)
{
    CedarDecContext *s = avctx->priv_data;
    AVFrame *frame;

    if (s->nb_reserved == MAX_RESERVED) {   /* lost in the decoder: give up the oldest */
        av_frame_free(&s->reserved[0].frame);
        memmove(s->reserved, s->reserved + 1, --s->nb_reserved * sizeof(*s->reserved));
    }
    frame = av_frame_alloc();
    if (!frame)
        return;
    frame->format = format;
    frame->width = avctx->width;
    frame->height = avctx->height;
    if (get_buffer_for(avctx, frame, avctx->reordered_opaque) < 0) {
        av_frame_free(&frame);
        return;
    }
    s->reserved[s->nb_reserved++] = (Reserved) { avctx->reordered_opaque, frame };
}

/* The output frame for reordered_opaque: its reserved buffer if it matches, else a new one */
static int output_buffer(AVCodecContext *avctx, AVFrame *frame, int64_t reordered_opaque,
                         enum AVPixelFormat format, int width, int height)
{
    CedarDecContext *s = avctx->priv_data;

    for (int i = 0; i < s->nb_reserved; i++) {
        AVFrame *r = s->reserved[i].frame;
        if (s->reserved[i].reordered_opaque != reordered_opaque)
            continue;
        memmove(s->reserved + i, s->reserved + i + 1, (--s->nb_reserved - i) * sizeof(*s->reserved));
        if (r->format == format && r->width == width && r->height == height) {
            av_frame_move_ref(frame, r);
            av_frame_free(&r);
            return 0;
        }
        av_frame_free(&r);              /* size or format changed */
        break;
    }
    frame->format = format;
    frame->width = width;
    frame->height = height;
    return get_buffer_for(avctx, frame, reordered_opaque);
}

/* Give the next picture the smallest pending input timestamp (see CedarPktInfo). */
static CedarPktInfo cedar_take_pts(CedarDecContext *s)
{
    CedarPktInfo info = { AV_NOPTS_VALUE, 0, 0 };
    int min = 0;

    s->nb_pictures++;
    if (!s->nb_pending) {
        s->nb_no_pts++;
        return info;
    }
    for (int i = 1; i < s->nb_pending; i++)
        if (s->pending[i].pts < s->pending[min].pts)
            min = i;
    info = s->pending[min];
    s->pending[min] = s->pending[--s->nb_pending];
    return info;
}

/* Copy the next decoded picture, if any, into frame. Pictures of discarded packets (edit-list
 * pre-roll) go straight back to the decoder. */
static int cedar_output(AVCodecContext *avctx, AVFrame *frame)
{
    CedarDecContext *s = avctx->priv_data;
    VideoPicture *pic;
    CedarPktInfo info;
    int left, top, width, height, ret;

    for (;;) {
        pic = RequestPicture(s->dec, 0);
        if (!pic)
            return AVERROR(EAGAIN);
        info = cedar_take_pts(s);
        if (!info.discard)
            break;
        s->nb_discarded++;
        ReturnPicture(s->dec, pic);
    }

    left = pic->nLeftOffset;
    top = pic->nTopOffset;
    width = (pic->nRightOffset > left ? pic->nRightOffset : pic->nWidth) - left;
    height = (pic->nBottomOffset > top ? pic->nBottomOffset : pic->nHeight) - top;
    if (avctx->width != width || avctx->height != height) {
        ret = ff_set_dimensions(avctx, width, height);
        if (ret < 0)
            goto out;
    }

    ret = output_buffer(avctx, frame, info.reordered_opaque, AV_PIX_FMT_NV21, width, height);
    if (ret < 0)
        goto out;

    /* The hardware wrote to memory the CPU may still have cached */
    CdcMemFlushCache(s->memops, pic->pData0, pic->nLineStride * pic->nHeight);
    CdcMemFlushCache(s->memops, pic->pData1, pic->nLineStride * pic->nHeight / 2);
    av_image_copy_plane(frame->data[0], frame->linesize[0],
                        (const uint8_t *)pic->pData0 + top * pic->nLineStride + left,
                        pic->nLineStride, width, height);
    av_image_copy_plane(frame->data[1], frame->linesize[1],
                        (const uint8_t *)pic->pData1 + (top / 2) * pic->nLineStride + (left & ~1),
                        pic->nLineStride, (width + 1) & ~1, (height + 1) / 2);

    frame->pts = info.pts;
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->flags &= ~AV_FRAME_FLAG_DISCARD;     /* ff_get_buffer() took it from the last packet */
    if (pic->bIsProgressive == 0) {
        frame->interlaced_frame = 1;
        frame->top_field_first = !!pic->bTopFieldFirst;
    }
    if (pic->bFrameErrorFlag)
        frame->decode_error_flags |= FF_DECODE_ERROR_INVALID_BITSTREAM;
    ret = 0;
out:
    ReturnPicture(s->dec, pic);
    return ret;
}

/* Put the pending packet into the decoder's stream buffer; AVERROR(EAGAIN) if it's full. */
static int cedar_submit(AVCodecContext *avctx)
{
    CedarDecContext *s = avctx->priv_data;
    AVPacket *pkt = s->pkt;
    VideoStreamDataInfo data;
    char *buf0, *buf1;
    int size0, size1, first;

    if (RequestVideoStreamBuffer(s->dec, pkt->size, &buf0, &size0, &buf1, &size1, 0)
        || size0 + size1 < pkt->size)
        return AVERROR(EAGAIN);
    first = FFMIN(pkt->size, size0);
    memcpy(buf0, pkt->data, first);
    if (first < pkt->size)
        memcpy(buf1, pkt->data + first, pkt->size - first);

    memset(&data, 0, sizeof(data));
    data.pData = buf0;
    data.nLength = pkt->size;
    data.nPts = -1;
    if (pkt->pts != AV_NOPTS_VALUE || pkt->dts != AV_NOPTS_VALUE) {
        if (s->nb_pending == MAX_PENDING) /* frames the decoder dropped: forget the oldest */
            memmove(s->pending, s->pending + 1, --s->nb_pending * sizeof(*s->pending));
        s->pending[s->nb_pending++] = (CedarPktInfo) {
            .pts              = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts,
            .reordered_opaque = avctx->reordered_opaque, /* set by the caller for this packet */
            .discard          = !!(pkt->flags & AV_PKT_FLAG_DISCARD),
        };
    }
    data.bIsFirstPart = 1;
    data.bIsLastPart = 1;
    reserve_frame(avctx, AV_PIX_FMT_NV21);
    if (SubmitVideoStreamData(s->dec, &data, 0)) {
        av_log(avctx, AV_LOG_ERROR, "SubmitVideoStreamData failed\n");
        return AVERROR_EXTERNAL;
    }
    av_packet_unref(pkt);
    s->pkt_pending = 0;
    s->nb_submitted++;
    return 0;
}

/* Software fallback: feed the inner decoder, copy its frames into ours (gst-libav must allocate
 * the frames it gets back) */
static int sw_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    CedarDecContext *s = avctx->priv_data;
    AVFrame *src = s->sw_frame;
    int ret;

    for (;;) {
        ret = avcodec_receive_frame(s->sw, src);
        if (ret == 0)
            break;
        if (ret != AVERROR(EAGAIN))
            return ret;                 /* AVERROR_EOF once drained, or an error */
        if (s->sw_drain_sent)
            return AVERROR_EOF;
        ret = ff_decode_get_packet(avctx, s->pkt);
        if (ret == AVERROR_EOF) {
            s->sw_drain_sent = 1;
            ret = avcodec_send_packet(s->sw, NULL);
        } else if (ret < 0) {
            return ret;                 /* EAGAIN: no input yet */
        } else {
            s->sw->reordered_opaque = avctx->reordered_opaque;
            reserve_frame(avctx, avctx->pix_fmt);
            ret = avcodec_send_packet(s->sw, s->pkt);
            av_packet_unref(s->pkt);
        }
        if (ret < 0 && ret != AVERROR(EAGAIN))
            return ret;
    }

    if (avctx->width != src->width || avctx->height != src->height) {
        ret = ff_set_dimensions(avctx, src->width, src->height);
        if (ret < 0)
            goto out;
    }
    avctx->pix_fmt = src->format;
    ret = output_buffer(avctx, frame, src->reordered_opaque, src->format, src->width, src->height);
    if (ret < 0)
        goto out;
    ret = av_frame_copy(frame, src);
    if (ret < 0)
        goto out;
    /* the frame's own properties; not av_frame_copy_props(), which would also take src's opaque
     * (gst-libav keeps its own there) */
    frame->pts = src->pts;
    frame->pkt_dts = src->pkt_dts;
    frame->key_frame = src->key_frame;
    frame->pict_type = src->pict_type;
    frame->sample_aspect_ratio = src->sample_aspect_ratio;
    frame->interlaced_frame = src->interlaced_frame;
    frame->top_field_first = src->top_field_first;
    frame->repeat_pict = src->repeat_pict;
    frame->color_range = src->color_range;
    frame->color_primaries = src->color_primaries;
    frame->color_trc = src->color_trc;
    frame->colorspace = src->colorspace;
    frame->chroma_location = src->chroma_location;
    frame->decode_error_flags = src->decode_error_flags;
    frame->flags = src->flags;
    ret = 0;
out:
    av_frame_unref(src);
    return ret;
}

static int cedar_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    CedarDecContext *s = avctx->priv_data;
    int ret, need_input = 0;

    if (s->sw)
        return sw_receive_frame(avctx, frame);

    for (int step = 0; step < MAX_DECODE_STEPS; step++) {
        ret = cedar_output(avctx, frame);
        if (ret != AVERROR(EAGAIN))
            return ret;

        /* Feed input: one packet at a time, at most MAX_PENDING ahead of the output */
        if (!s->eof && (s->pkt_pending || s->nb_pending < MAX_PENDING)) {
            if (!s->pkt_pending) {
                ret = ff_decode_get_packet(avctx, s->pkt);
                if (ret == AVERROR_EOF)
                    s->eof = 1;
                else if (ret < 0 && ret != AVERROR(EAGAIN))
                    return ret;
                else if (ret == 0)
                    s->pkt_pending = 1;
                need_input = ret == AVERROR(EAGAIN);
            }
            if (s->pkt_pending) {
                ret = cedar_submit(avctx);
                if (ret < 0 && ret != AVERROR(EAGAIN))
                    return ret;
            }
        }

        ret = DecodeVideoStream(s->dec, s->eof, 0, 0, 0);
        switch (ret) {
        case VDECODE_RESULT_FRAME_DECODED:
        case VDECODE_RESULT_KEYFRAME_DECODED:
        case VDECODE_RESULT_OK:
        case VDECODE_RESULT_CONTINUE:
        case VDECODE_RESULT_NO_FRAME_BUFFER:
        case VDECODE_RESULT_RESOLUTION_CHANGE:
            continue;
        case VDECODE_RESULT_NO_BITSTREAM:
            if (s->eof) {
                ret = cedar_output(avctx, frame);
                return ret == AVERROR(EAGAIN) ? AVERROR_EOF : ret;
            }
            if (need_input && !s->pkt_pending)
                return AVERROR(EAGAIN);
            continue;
        default:
            av_log(avctx, AV_LOG_ERROR, "DecodeVideoStream failed: %d\n", ret);
            return AVERROR_EXTERNAL;
        }
    }
    return AVERROR(EAGAIN);
}

static void cedar_flush(AVCodecContext *avctx)
{
    CedarDecContext *s = avctx->priv_data;
    av_packet_unref(s->pkt);
    clear_reserved(s);
    if (s->sw) {
        avcodec_flush_buffers(s->sw);
        s->sw_drain_sent = 0;
        return;
    }
    ResetVideoDecoder(s->dec);
    s->pkt_pending = 0;
    s->eof = 0;
    s->nb_pending = 0;
}

static const AVClass cedar_h264_dec_class = {
    .class_name = "h264_cedar",
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_h264_cedar_decoder = {
    .p.name         = "h264_cedar",
    .p.long_name    = NULL_IF_CONFIG_SMALL("H.264 (Allwinner Cedar hardware decoder, software fallback)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(CedarDecContext),
    .init           = cedar_init,
    .close          = cedar_close,
    FF_CODEC_RECEIVE_FRAME_CB(cedar_receive_frame),
    .flush          = cedar_flush,
    .p.priv_class   = &cedar_h264_dec_class,
    /* no AV_CODEC_CAP_HARDWARE: gst-libav skips those, and frames are ordinary memory anyway */
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,
    .bsfs           = "h264_mp4toannexb",
    .p.wrapper_name = "cedar",
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
