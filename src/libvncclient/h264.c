/*
 * h264.c - handle H.264 encoding.
 *
 * This file shouldn't be compiled directly. It is included multiple times by
 * rfbclient.c, each time with a different definition of the macro BPP.
 */

#ifdef LIBVNCSERVER_HAVE_LIBAVCODEC

#ifndef LIBVNCSERVER_HAVE_LIBSWSCALE
#error "H.264 support requires libswscale"
#endif

#include <limits.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#ifndef LIBVNCCLIENT_H264_COMMON
#define LIBVNCCLIENT_H264_COMMON

static rfbBool rfbClientH264EnsureDecoder(rfbClient *client);
static rfbBool rfbClientH264EnsureBgraBuffer(rfbClient *client, int width, int height);
static rfbBool rfbClientH264EnsureRawBuffer(rfbClient *client, int width, int height, int destBpp);
static rfbBool rfbClientH264BlitFrame(rfbClient *client, AVFrame *frame,
                                      int rx, int ry, int rw, int rh, int destBpp);
static void rfbClientH264PackPixel(const rfbPixelFormat *fmt, uint8_t *dst,
                                   uint8_t r, uint8_t g, uint8_t b);

static void
rfbClientH264InitLogging(void)
{
    static rfbBool initialized = FALSE;
    if (!initialized) {
        av_log_set_level(AV_LOG_QUIET);
        initialized = TRUE;
    }
}

void
rfbClientH264ReleaseDecoder(rfbClient *client)
{
    if (client == NULL) {
        return;
    }

    av_packet_free(&client->h264Packet);
    av_frame_free(&client->h264Frame);
    if (client->h264Decoder != NULL) {
        avcodec_free_context(&client->h264Decoder);
    }
    client->h264Decoder = NULL;

#ifdef LIBVNCSERVER_HAVE_LIBSWSCALE
    if (client->h264SwsContext != NULL) {
        sws_freeContext(client->h264SwsContext);
        client->h264SwsContext = NULL;
    }
#endif

    free(client->h264BgraBuffer);
    client->h264BgraBuffer = NULL;
    client->h264BgraBufferSize = 0;
    client->h264CodecWidth = 0;
    client->h264CodecHeight = 0;
    client->h264LastFormat = -1;
}

static rfbBool
rfbClientH264EnsureDecoder(rfbClient *client)
{
    rfbClientH264InitLogging();

    if (client->h264Decoder == NULL) {
        const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (decoder == NULL) {
            rfbClientLog("H.264 decoder not found\n");
            return FALSE;
        }

        client->h264Decoder = avcodec_alloc_context3(decoder);
        if (client->h264Decoder == NULL) {
            rfbClientLog("Failed to allocate H.264 decoder context\n");
            return FALSE;
        }

        client->h264Decoder->thread_count = 1; /* Disable frame threading to avoid delayed output */
#ifdef FF_THREAD_SLICE
        client->h264Decoder->thread_type = FF_THREAD_SLICE;
#else
        client->h264Decoder->thread_type = 0;
#endif
        client->h264Decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;

        if (avcodec_open2(client->h264Decoder, decoder, NULL) < 0) {
            rfbClientLog("Failed to open H.264 decoder\n");
            avcodec_free_context(&client->h264Decoder);
            return FALSE;
        }
    }

    if (client->h264Frame == NULL) {
        client->h264Frame = av_frame_alloc();
        if (client->h264Frame == NULL) {
            rfbClientLog("Failed to allocate H.264 frame\n");
            return FALSE;
        }
    }

    if (client->h264Packet == NULL) {
        client->h264Packet = av_packet_alloc();
        if (client->h264Packet == NULL) {
            rfbClientLog("Failed to allocate H.264 packet\n");
            return FALSE;
        }
    }

    return TRUE;
}

static rfbBool
rfbClientH264EnsureBgraBuffer(rfbClient *client, int width, int height)
{
    if (width <= 0 || height <= 0) {
        rfbClientLog("Invalid H.264 frame dimensions %dx%d\n", width, height);
        return FALSE;
    }

    size_t pixels = (size_t)width * (size_t)height;
    if (pixels == 0 || pixels > SIZE_MAX / 4) {
        rfbClientLog("H.264 frame too large (%dx%d)\n", width, height);
        return FALSE;
    }

    size_t required = pixels * 4;
    if (client->h264BgraBufferSize < required) {
        uint8_t *newBuffer = (uint8_t *)realloc(client->h264BgraBuffer, required);
        if (newBuffer == NULL) {
            rfbClientLog("Failed to allocate %zu bytes for H.264 conversion buffer\n", required);
            return FALSE;
        }
        client->h264BgraBuffer = newBuffer;
        client->h264BgraBufferSize = required;
    }

    return TRUE;
}

static rfbBool
rfbClientH264EnsureRawBuffer(rfbClient *client, int width, int height, int destBpp)
{
    if (width <= 0 || height <= 0 || destBpp <= 0) {
        rfbClientLog("Invalid framebuffer dimensions for H.264 blit: %dx%d @ %d bpp\n",
                     width, height, destBpp);
        return FALSE;
    }

    size_t bytesPerPixel = (size_t)destBpp / 8;
    size_t pixels = (size_t)width * (size_t)height;

    if (pixels == 0 || bytesPerPixel == 0 || pixels > SIZE_MAX / bytesPerPixel) {
        rfbClientLog("Requested framebuffer update too large for H.264 (%dx%d)\n",
                     width, height);
        return FALSE;
    }

    size_t required = pixels * bytesPerPixel;
    if (required > (size_t)INT_MAX) {
        rfbClientLog("H.264 framebuffer update exceeds maximum buffer size\n");
        return FALSE;
    }

    if (client->raw_buffer_size < (int)required) {
        char *newBuffer = (char *)realloc(client->raw_buffer, required);
        if (newBuffer == NULL) {
            rfbClientLog("Failed to allocate %zu bytes for framebuffer buffer\n", required);
            return FALSE;
        }
        client->raw_buffer = newBuffer;
        client->raw_buffer_size = (int)required;
    }

    return TRUE;
}

static void
rfbClientH264PackPixel(const rfbPixelFormat *fmt, uint8_t *dst,
                       uint8_t r, uint8_t g, uint8_t b)
{
    int i;
    const uint32_t red = ((uint32_t)r * fmt->redMax + 127) / 255;
    const uint32_t green = ((uint32_t)g * fmt->greenMax + 127) / 255;
    const uint32_t blue = ((uint32_t)b * fmt->blueMax + 127) / 255;

    uint32_t pixel = (red << fmt->redShift) |
                     (green << fmt->greenShift) |
                     (blue << fmt->blueShift);

    int bytes = fmt->bitsPerPixel / 8;
    if (fmt->bigEndian) {
        for (i = 0; i < bytes; ++i) {
            dst[i] = (uint8_t)((pixel >> ((bytes - 1 - i) * 8)) & 0xFF);
        }
    } else {
        for (i = 0; i < bytes; ++i) {
            dst[i] = (uint8_t)((pixel >> (i * 8)) & 0xFF);
        }
    }
}

static rfbBool
rfbClientH264BlitFrame(rfbClient *client, AVFrame *frame,
                       int rx, int ry, int rw, int rh, int destBpp)
{
    if (!rfbClientH264EnsureBgraBuffer(client, frame->width, frame->height)) {
        return FALSE;
    }

#ifdef LIBVNCSERVER_HAVE_LIBSWSCALE
    struct SwsContext *sws = sws_getCachedContext(client->h264SwsContext,
                                                 frame->width, frame->height,
                                                 (enum AVPixelFormat)frame->format,
                                                 frame->width, frame->height,
                                                 AV_PIX_FMT_BGRA,
                                                 SWS_BILINEAR, NULL, NULL, NULL);
    if (sws == NULL) {
        rfbClientLog("Failed to create H.264 colorspace converter\n");
        return FALSE;
    }
    client->h264SwsContext = sws;

    uint8_t *destData[4] = { client->h264BgraBuffer, NULL, NULL, NULL };
    int destLinesize[4] = { frame->width * 4, 0, 0, 0 };

    int scaled = sws_scale(sws, (const uint8_t * const *)frame->data,
                           frame->linesize, 0, frame->height,
                           destData, destLinesize);
    if (scaled < frame->height) {
        rfbClientLog("H.264 conversion produced %d lines (expected %d)\n",
                     scaled, frame->height);
        return FALSE;
    }
#else
    (void)rw;
    (void)rh;
#endif

    if (!rfbClientH264EnsureRawBuffer(client, frame->width, frame->height, destBpp)) {
        return FALSE;
    }

    if (!client->format.trueColour) {
        rfbClientLog("H.264 encoding requires true-colour pixel format\n");
        return FALSE;
    }

    if (frame->width != rw || frame->height != rh) {
        rfbClientLog("H.264 frame size (%dx%d) differs from rectangle (%dx%d)\n",
                     frame->width, frame->height, rw, rh);
    }

    int bytesPerPixel = destBpp / 8;
    int y, x;
    size_t rowSize = (size_t)frame->width * (size_t)bytesPerPixel;
    uint8_t *dst = (uint8_t *)client->raw_buffer;
    const uint8_t *src = client->h264BgraBuffer;

    for (y = 0; y < frame->height; ++y) {
        const uint8_t *srcRow = src + (size_t)y * frame->width * 4;
        uint8_t *dstRow = dst + (size_t)y * rowSize;
        for (x = 0; x < frame->width; ++x) {
            const uint8_t *pix = srcRow + x * 4;
            rfbClientH264PackPixel(&client->format, dstRow + x * bytesPerPixel,
                                   pix[2], pix[1], pix[0]);
        }
    }

    client->GotBitmap(client, (uint8_t *)client->raw_buffer,
                      rx, ry, frame->width, frame->height);

    client->h264CodecWidth = frame->width;
    client->h264CodecHeight = frame->height;
    client->h264LastFormat = frame->format;

    return TRUE;
}

#endif /* LIBVNCCLIENT_H264_COMMON */

#define HandleH264BPP CONCAT2E(HandleH264, BPP)

static rfbBool
HandleH264BPP(rfbClient *client, int rx, int ry, int rw, int rh)
{
    rfbH264Header header;
    if (!ReadFromRFBServer(client, (char *)&header, sz_rfbH264Header)) {
        return FALSE;
    }

    uint32_t dataBytes = rfbClientSwap32IfLE(header.length);
    uint32_t flags = rfbClientSwap32IfLE(header.flags);

    if (!rfbClientH264EnsureDecoder(client)) {
        return FALSE;
    }

    if (flags == rfbH264FlagResetContext) {
        avcodec_flush_buffers(client->h264Decoder);
        client->h264CodecWidth = 0;
        client->h264CodecHeight = 0;
    } else if (flags != rfbH264FlagNone) {
        rfbClientLog("Unknown H.264 flag value %u\n", flags);
    }

    AVPacket *packet = client->h264Packet;
    AVCodecContext *decoder = client->h264Decoder;
    AVFrame *frame = client->h264Frame;

    int ret;
    if (dataBytes == 0) {
        ret = avcodec_send_packet(decoder, NULL);
    } else {
        if (av_new_packet(packet, dataBytes) < 0) {
            rfbClientLog("Failed to allocate H.264 packet of %u bytes\n", dataBytes);
            return FALSE;
        }

        if (!ReadFromRFBServer(client, (char *)packet->data, dataBytes)) {
            av_packet_unref(packet);
            return FALSE;
        }

        ret = avcodec_send_packet(decoder, packet);
        av_packet_unref(packet);
    }

    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        rfbClientLog("H.264 decoder rejected packet (%d)\n", ret);
        return FALSE;
    }

    rfbBool gotFrame = FALSE;

    while ((ret = avcodec_receive_frame(decoder, frame)) >= 0) {
        gotFrame = TRUE;
        if (!rfbClientH264BlitFrame(client, frame, rx, ry, rw, rh, BPP)) {
            av_frame_unref(frame);
            return FALSE;
        }
        av_frame_unref(frame);
    }

    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        rfbClientLog("H.264 decoder error (%d)\n", ret);
        return FALSE;
    }

    return TRUE;
}

#undef HandleH264BPP

#endif /* LIBVNCSERVER_HAVE_LIBAVCODEC */
