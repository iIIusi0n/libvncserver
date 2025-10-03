/*
 * h264.c - server-side H.264 encoding helpers.
 */

#ifdef LIBVNCSERVER_HAVE_LIBAVCODEC

#ifndef LIBVNCSERVER_HAVE_LIBSWSCALE
#error "H.264 support requires libswscale"
#endif

#include <limits.h>
#include <string.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <rfb/rfb.h>

#define H264_LOG_PREFIX "H264: "

static rfbBool rfbClientH264EnsureEncoder(rfbClientPtr cl, int width, int height);
static rfbBool rfbClientH264EnsureRgbBuffer(rfbClientPtr cl, size_t required);
static rfbBool rfbClientH264FillBgra(rfbClientPtr cl);
static rfbBool rfbClientH264EncodeFrame(rfbClientPtr cl, rfbBool forceKeyframe, size_t *encodedSizeOut);
static rfbBool rfbClientH264EnsureEncodeCapacity(rfbClientPtr cl, size_t required);
static rfbBool rfbClientH264PrependConfig(rfbClientPtr cl, rfbBool forceKeyframe, size_t *encodedSize);
static rfbBool rfbClientH264AppendPacket(rfbClientPtr cl, const AVPacket *packet, size_t *encodedSize);
static rfbBool rfbClientH264ExtradataToAnnexB(const uint8_t *extra, size_t extraSize, uint8_t **out, size_t *outSize);
static rfbBool rfbClientH264PacketNeedsAnnexB(const AVPacket *packet);
static int rfbClientH264GetNalLengthSize(const AVCodecContext *ctx);

static const uint8_t h264StartCode[4] = { 0x00, 0x00, 0x00, 0x01 };

static void
rfbClientH264InitLogging(void)
{
    static rfbBool initialized = FALSE;
    if (!initialized) {
        av_log_set_level(AV_LOG_QUIET);
        initialized = TRUE;
    }
}

static rfbBool
rfbClientH264EnsureEncodeCapacity(rfbClientPtr cl, size_t required)
{
    if (required > cl->h264EncodeBufferSize) {
        uint8_t *buf = (uint8_t *)realloc(cl->h264EncodeBuffer, required);
        if (buf == NULL) {
            rfbLog(H264_LOG_PREFIX "failed to allocate %zu bytes for encoded buffer\n", required);
            return FALSE;
        }
        cl->h264EncodeBuffer = buf;
        cl->h264EncodeBufferSize = required;
    }
    return TRUE;
}

static rfbBool
rfbClientH264AppendBytes(rfbClientPtr cl, const uint8_t *data, size_t len, size_t *offset)
{
    if (!rfbClientH264EnsureEncodeCapacity(cl, *offset + len)) {
        return FALSE;
    }
    memcpy(cl->h264EncodeBuffer + *offset, data, len);
    *offset += len;
    return TRUE;
}

static rfbBool
rfbClientH264AppendStartCode(rfbClientPtr cl, size_t *offset)
{
    return rfbClientH264AppendBytes(cl, h264StartCode, sizeof(h264StartCode), offset);
}

static int
rfbClientH264GetNalLengthSize(const AVCodecContext *ctx)
{
    if (ctx->extradata_size >= 5 && ctx->extradata != NULL && ctx->extradata[0] == 1) {
        return (ctx->extradata[4] & 0x03) + 1;
    }
    return 4;
}

void
rfbClientH264ReleaseEncoder(rfbClientPtr cl)
{
    if (cl == NULL) {
        return;
    }

    av_packet_free(&cl->h264Packet);
    av_frame_free(&cl->h264Frame);
    if (cl->h264Encoder != NULL) {
        avcodec_free_context(&cl->h264Encoder);
    }
    cl->h264Encoder = NULL;

    if (cl->h264SwsContext != NULL) {
        sws_freeContext(cl->h264SwsContext);
        cl->h264SwsContext = NULL;
    }

    free(cl->h264RgbBuffer);
    cl->h264RgbBuffer = NULL;
    cl->h264RgbBufferSize = 0;

    free(cl->h264EncodeBuffer);
    cl->h264EncodeBuffer = NULL;
    cl->h264EncodeBufferSize = 0;

    cl->h264CodecWidth = 0;
    cl->h264CodecHeight = 0;
    cl->h264FramePts = 0;
    cl->h264ForceKeyframe = FALSE;
    cl->h264SentConfig = FALSE;
}

void
rfbClientH264SetBitrate(rfbClientPtr cl, int64_t bitRate)
{
    if (cl == NULL) {
        return;
    }

    cl->h264BitRate = bitRate;

    if (cl->h264Encoder != NULL) {
        int64_t target = bitRate;
        if (target <= 0 && cl->h264CodecWidth > 0 && cl->h264CodecHeight > 0) {
            target = (int64_t)cl->h264CodecWidth * cl->h264CodecHeight * 4;
        }
        if (target > 0 && cl->h264Encoder->bit_rate != target) {
            rfbClientH264ReleaseEncoder(cl);
        }
    }
}

static rfbBool
rfbClientH264EnsureEncoder(rfbClientPtr cl, int width, int height)
{
    rfbClientH264InitLogging();

    if (!cl->scaledScreen->serverFormat.trueColour) {
        rfbLog(H264_LOG_PREFIX "server pixel format must be true-colour for H.264\n");
        return FALSE;
    }

    if (width <= 0 || height <= 0) {
        rfbLog(H264_LOG_PREFIX "invalid frame size %dx%d\n", width, height);
        return FALSE;
    }

    if (cl->h264Encoder != NULL &&
        (cl->h264CodecWidth != width || cl->h264CodecHeight != height)) {
        rfbClientH264ReleaseEncoder(cl);
    }

    if (cl->h264Encoder == NULL) {
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        AVCodecContext *ctx;

        if (codec == NULL) {
            rfbLog(H264_LOG_PREFIX "H.264 encoder not available\n");
            return FALSE;
        }

        ctx = avcodec_alloc_context3(codec);
        if (ctx == NULL) {
            rfbLog(H264_LOG_PREFIX "failed to allocate encoder context\n");
            return FALSE;
        }

        ctx->width = width;
        ctx->height = height;
        ctx->time_base = (AVRational){1, 30};
        ctx->framerate = (AVRational){30, 1};
        ctx->gop_size = 30;
        ctx->max_b_frames = 0;
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->thread_count = 1; /* Disable frame threading to avoid delayed output */
#ifdef FF_THREAD_SLICE
        ctx->thread_type = FF_THREAD_SLICE;
#endif
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

        int64_t bitRate = cl->h264BitRate;
        if (bitRate <= 0) {
            bitRate = (int64_t)width * height * 4; /* ~4 bits per pixel */
        }
        ctx->bit_rate = bitRate;

        av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);

        if (avcodec_open2(ctx, codec, NULL) < 0) {
            rfbLog(H264_LOG_PREFIX "failed to open encoder\n");
            avcodec_free_context(&ctx);
            return FALSE;
        }

        cl->h264Encoder = ctx;
        cl->h264FramePts = 0;

        cl->h264Frame = av_frame_alloc();
        if (cl->h264Frame == NULL) {
            rfbLog(H264_LOG_PREFIX "failed to allocate frame\n");
            rfbClientH264ReleaseEncoder(cl);
            return FALSE;
        }

        cl->h264Frame->format = ctx->pix_fmt;
        cl->h264Frame->width = ctx->width;
        cl->h264Frame->height = ctx->height;

        if (av_frame_get_buffer(cl->h264Frame, 32) < 0) {
            rfbLog(H264_LOG_PREFIX "failed to allocate frame buffer\n");
            rfbClientH264ReleaseEncoder(cl);
            return FALSE;
        }

        cl->h264Packet = av_packet_alloc();
        if (cl->h264Packet == NULL) {
            rfbLog(H264_LOG_PREFIX "failed to allocate packet\n");
            rfbClientH264ReleaseEncoder(cl);
            return FALSE;
        }

        cl->h264CodecWidth = width;
        cl->h264CodecHeight = height;
        cl->h264ForceKeyframe = TRUE;
        cl->h264SentConfig = FALSE;
    }

    return TRUE;
}

static rfbBool
rfbClientH264EnsureRgbBuffer(rfbClientPtr cl, size_t required)
{
    if (required > cl->h264RgbBufferSize) {
        uint8_t *buf = (uint8_t *)realloc(cl->h264RgbBuffer, required);
        if (buf == NULL) {
            rfbLog(H264_LOG_PREFIX "failed to allocate RGB buffer (%zu bytes)\n", required);
            return FALSE;
        }
        cl->h264RgbBuffer = buf;
        cl->h264RgbBufferSize = required;
    }
    return TRUE;
}

static inline uint32_t
rfbClientH264ReadPixel(const rfbPixelFormat *fmt, const uint8_t *src)
{
    uint32_t value = 0;

    switch (fmt->bitsPerPixel) {
    case 32:
        if (fmt->bigEndian) {
            value = ((uint32_t)src[0] << 24) |
                    ((uint32_t)src[1] << 16) |
                    ((uint32_t)src[2] << 8) |
                    (uint32_t)src[3];
        } else {
            value = *((const uint32_t *)src);
        }
        break;
    case 24:
        if (fmt->bigEndian) {
            value = ((uint32_t)src[0] << 16) |
                    ((uint32_t)src[1] << 8) |
                    (uint32_t)src[2];
        } else {
            value = ((uint32_t)src[2] << 16) |
                    ((uint32_t)src[1] << 8) |
                    (uint32_t)src[0];
        }
        break;
    case 16:
        if (fmt->bigEndian) {
            value = ((uint32_t)src[0] << 8) | (uint32_t)src[1];
        } else {
            value = *((const uint16_t *)src);
        }
        break;
    case 8:
        value = src[0];
        break;
    default:
        break;
    }

    return value;
}

static inline uint8_t
rfbClientH264ScaleComponent(uint32_t value, uint32_t max)
{
    if (max == 0) {
        return 0;
    }
    return (uint8_t)((value * 255U + (max / 2U)) / max);
}

static rfbBool
rfbClientH264PacketNeedsAnnexB(const AVPacket *packet)
{
    if (packet->size >= 4 && packet->data != NULL) {
        if (packet->data[0] == 0x00 && packet->data[1] == 0x00 &&
            packet->data[2] == 0x00 && packet->data[3] == 0x01) {
            return FALSE;
        }
    }
    return TRUE;
}

static rfbBool
rfbClientH264ExtradataToAnnexB(const uint8_t *extra, size_t extraSize, uint8_t **out, size_t *outSize)
{
    *out = NULL;
    *outSize = 0;

    if (extra == NULL || extraSize == 0) {
        return TRUE;
    }

    if (extraSize >= 4 && extra[0] == 0x00 && extra[1] == 0x00 && extra[2] == 0x00 && extra[3] == 0x01) {
        uint8_t *buf = (uint8_t *)malloc(extraSize);
        if (buf == NULL) {
            return FALSE;
        }
        memcpy(buf, extra, extraSize);
        *out = buf;
        *outSize = extraSize;
        return TRUE;
    }

    if (extraSize < 7 || extra[0] != 1) {
        return FALSE;
    }

    size_t pos = 5;
    uint8_t numSps = extra[pos++] & 0x1F;
    size_t capacity = extraSize * 4 + 16;
    size_t offset = 0;
    uint8_t *buf = (uint8_t *)malloc(capacity);
    if (buf == NULL) {
        return FALSE;
    }

    uint8_t i;

    for (i = 0; i < numSps; ++i) {
        if (pos + 2 > extraSize) {
            free(buf);
            return FALSE;
        }
        uint16_t nalSize = ((uint16_t)extra[pos] << 8) | extra[pos + 1];
        pos += 2;
        if (pos + nalSize > extraSize) {
            free(buf);
            return FALSE;
        }
        if (offset + sizeof(h264StartCode) + nalSize > capacity) {
            size_t newCap = capacity * 2 + sizeof(h264StartCode) + nalSize;
            uint8_t *tmp = (uint8_t *)realloc(buf, newCap);
            if (tmp == NULL) {
                free(buf);
                return FALSE;
            }
            buf = tmp;
            capacity = newCap;
        }
        memcpy(buf + offset, h264StartCode, sizeof(h264StartCode));
        offset += sizeof(h264StartCode);
        memcpy(buf + offset, extra + pos, nalSize);
        offset += nalSize;
        pos += nalSize;
    }

    if (pos + 1 > extraSize) {
        free(buf);
        return FALSE;
    }
    uint8_t numPps = extra[pos++];
    for (i = 0; i < numPps; ++i) {
        if (pos + 2 > extraSize) {
            free(buf);
            return FALSE;
        }
        uint16_t nalSize = ((uint16_t)extra[pos] << 8) | extra[pos + 1];
        pos += 2;
        if (pos + nalSize > extraSize) {
            free(buf);
            return FALSE;
        }
        if (offset + sizeof(h264StartCode) + nalSize > capacity) {
            size_t newCap = capacity * 2 + sizeof(h264StartCode) + nalSize;
            uint8_t *tmp = (uint8_t *)realloc(buf, newCap);
            if (tmp == NULL) {
                free(buf);
                return FALSE;
            }
            buf = tmp;
            capacity = newCap;
        }
        memcpy(buf + offset, h264StartCode, sizeof(h264StartCode));
        offset += sizeof(h264StartCode);
        memcpy(buf + offset, extra + pos, nalSize);
        offset += nalSize;
        pos += nalSize;
    }

    if (pos + 1 <= extraSize) {
        uint8_t numSpsExt = extra[pos++];
        for (i = 0; i < numSpsExt; ++i) {
            if (pos + 2 > extraSize) {
                free(buf);
                return FALSE;
            }
            uint16_t nalSize = ((uint16_t)extra[pos] << 8) | extra[pos + 1];
            pos += 2;
            if (pos + nalSize > extraSize) {
                free(buf);
                return FALSE;
            }
            if (offset + sizeof(h264StartCode) + nalSize > capacity) {
                size_t newCap = capacity * 2 + sizeof(h264StartCode) + nalSize;
                uint8_t *tmp = (uint8_t *)realloc(buf, newCap);
                if (tmp == NULL) {
                    free(buf);
                    return FALSE;
                }
                buf = tmp;
                capacity = newCap;
            }
            memcpy(buf + offset, h264StartCode, sizeof(h264StartCode));
            offset += sizeof(h264StartCode);
            memcpy(buf + offset, extra + pos, nalSize);
            offset += nalSize;
            pos += nalSize;
        }
    }

    *out = buf;
    *outSize = offset;
    return TRUE;
}

static rfbBool
rfbClientH264AppendPacket(rfbClientPtr cl, const AVPacket *packet, size_t *encodedSize)
{
    if (packet->size <= 0) {
        return TRUE;
    }

    if (!rfbClientH264PacketNeedsAnnexB(packet)) {
        if (!rfbClientH264EnsureEncodeCapacity(cl, *encodedSize + (size_t)packet->size)) {
            return FALSE;
        }
        memcpy(cl->h264EncodeBuffer + *encodedSize, packet->data, (size_t)packet->size);
        *encodedSize += (size_t)packet->size;
        return TRUE;
    }

    int nalLengthSize = rfbClientH264GetNalLengthSize(cl->h264Encoder);
    size_t pos = 0;
    const size_t packetSize = (size_t)packet->size;

    while (pos + nalLengthSize <= packetSize) {
        uint32_t nalSize = 0;
        const uint8_t *data = packet->data + pos;

        switch (nalLengthSize) {
        case 1:
            nalSize = data[0];
            break;
        case 2:
            nalSize = ((uint32_t)data[0] << 8) | data[1];
            break;
        case 3:
            nalSize = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
            break;
        default:
            nalSize = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                      ((uint32_t)data[2] << 8) | data[3];
            break;
        }

        pos += nalLengthSize;

        if (nalSize == 0 || pos + nalSize > packetSize) {
            return FALSE;
        }

        if (!rfbClientH264AppendStartCode(cl, encodedSize)) {
            return FALSE;
        }

        if (!rfbClientH264AppendBytes(cl, packet->data + pos, nalSize, encodedSize)) {
            return FALSE;
        }

        pos += nalSize;
    }

    return pos == packetSize;
}

static rfbBool
rfbClientH264PrependConfig(rfbClientPtr cl, rfbBool forceKeyframe, size_t *encodedSize)
{
    if (cl->h264SentConfig && !forceKeyframe) {
        return TRUE;
    }

    uint8_t *annexB = NULL;
    size_t annexSize = 0;

    if (!rfbClientH264ExtradataToAnnexB(cl->h264Encoder->extradata,
                                        cl->h264Encoder->extradata_size,
                                        &annexB,
                                        &annexSize)) {
        free(annexB);
        return FALSE;
    }

    if (annexSize == 0) {
        cl->h264SentConfig = TRUE;
        free(annexB);
        return TRUE;
    }

    if (!rfbClientH264EnsureEncodeCapacity(cl, *encodedSize + annexSize)) {
        free(annexB);
        return FALSE;
    }

    memmove(cl->h264EncodeBuffer + annexSize, cl->h264EncodeBuffer, *encodedSize);
    memcpy(cl->h264EncodeBuffer, annexB, annexSize);
    *encodedSize += annexSize;
    cl->h264SentConfig = TRUE;

    free(annexB);
    return TRUE;
}


static rfbBool
rfbClientH264FillBgra(rfbClientPtr cl)
{
    const rfbPixelFormat *fmt = &cl->scaledScreen->serverFormat;
    const uint8_t *fb = (const uint8_t *)cl->scaledScreen->frameBuffer;
    const int stride = cl->scaledScreen->paddedWidthInBytes;
    const int width = cl->h264CodecWidth;
    const int height = cl->h264CodecHeight;
    size_t required = (size_t)width * (size_t)height * 4;

    if (!rfbClientH264EnsureRgbBuffer(cl, required)) {
        return FALSE;
    }

    int y;
    for (y = 0; y < height; ++y) {
        const uint8_t *src = fb + (size_t)y * stride;
        uint8_t *dst = cl->h264RgbBuffer + (size_t)y * width * 4;
        int x;

        for (x = 0; x < width; ++x) {
            const uint8_t *p = src + (size_t)x * (fmt->bitsPerPixel / 8);
            uint32_t pix = rfbClientH264ReadPixel(fmt, p);
            uint32_t rawR = (pix >> fmt->redShift) & fmt->redMax;
            uint32_t rawG = (pix >> fmt->greenShift) & fmt->greenMax;
            uint32_t rawB = (pix >> fmt->blueShift) & fmt->blueMax;

            dst[x * 4 + 2] = rfbClientH264ScaleComponent(rawR, fmt->redMax);
            dst[x * 4 + 1] = rfbClientH264ScaleComponent(rawG, fmt->greenMax);
            dst[x * 4 + 0] = rfbClientH264ScaleComponent(rawB, fmt->blueMax);
            dst[x * 4 + 3] = 0xFF;
        }
    }

    return TRUE;
}

static rfbBool
rfbClientH264EncodeFrame(rfbClientPtr cl, rfbBool forceKeyframe, size_t *encodedSizeOut)
{
    AVCodecContext *ctx = cl->h264Encoder;
    AVFrame *frame = cl->h264Frame;
    AVPacket *packet = cl->h264Packet;
    size_t encodedSize = 0;
    int ret;

    if (av_frame_make_writable(frame) < 0) {
        rfbLog(H264_LOG_PREFIX "frame not writable\n");
        return FALSE;
    }

    frame->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
#ifdef AV_FRAME_FLAG_KEY
    if (forceKeyframe)
        frame->flags |= AV_FRAME_FLAG_KEY;
    else
        frame->flags &= ~AV_FRAME_FLAG_KEY;
#endif

    struct SwsContext *sws = sws_getCachedContext(
        cl->h264SwsContext,
        ctx->width,
        ctx->height,
        AV_PIX_FMT_BGRA,
        ctx->width,
        ctx->height,
        ctx->pix_fmt,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL);
    if (sws == NULL) {
        rfbLog(H264_LOG_PREFIX "failed to create colorspace converter\n");
        return FALSE;
    }
    cl->h264SwsContext = sws;

    uint8_t *srcData[4] = { cl->h264RgbBuffer, NULL, NULL, NULL };
    int srcStride[4] = { ctx->width * 4, 0, 0, 0 };

    sws_scale(sws, (const uint8_t * const *)srcData, srcStride, 0, ctx->height,
              frame->data, frame->linesize);

    frame->pts = cl->h264FramePts++;

    ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        rfbLog(H264_LOG_PREFIX "encoder rejected frame (%d)\n", ret);
        return FALSE;
    }

    for (;;) {
        ret = avcodec_receive_packet(ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            rfbLog(H264_LOG_PREFIX "encoder failed (%d)\n", ret);
            return FALSE;
        }

        if (!rfbClientH264AppendPacket(cl, packet, &encodedSize)) {
            av_packet_unref(packet);
            return FALSE;
        }
        av_packet_unref(packet);
    }

    if (encodedSize == 0) {
        rfbLog(H264_LOG_PREFIX "encoder produced no output\n");
        return FALSE;
    }

    *encodedSizeOut = encodedSize;
    return TRUE;
}

rfbBool
rfbSendRectEncodingH264(rfbClientPtr cl,
                        int x,
                        int y,
                        int w,
                        int h)
{
    size_t encodedSize = 0;
    rfbFramebufferUpdateRectHeader rect;
    rfbH264Header hdr;
    int rawEquivalent;

    (void)x;
    (void)y;

    const int width = cl->scaledScreen->width;
    const int height = cl->scaledScreen->height;

    (void)w;
    (void)h;

    if (!rfbClientH264EnsureEncoder(cl, width, height)) {
        return FALSE;
    }

    rfbBool forceKeyframe = cl->h264ForceKeyframe;

    if (!rfbClientH264FillBgra(cl)) {
        return FALSE;
    }

    if (!rfbClientH264EncodeFrame(cl, forceKeyframe, &encodedSize)) {
        return FALSE;
    }

    if (!rfbClientH264PrependConfig(cl, forceKeyframe, &encodedSize)) {
        return FALSE;
    }

    if (encodedSize > UINT32_MAX) {
        rfbLog(H264_LOG_PREFIX "encoded frame too large (%zu bytes)\n", encodedSize);
        return FALSE;
    }

    if (cl->ublen > 0 && !rfbSendUpdateBuf(cl)) {
        return FALSE;
    }

    rect.r.x = Swap16IfLE(0);
    rect.r.y = Swap16IfLE(0);
    rect.r.w = Swap16IfLE((uint16_t)width);
    rect.r.h = Swap16IfLE((uint16_t)height);
    rect.encoding = Swap32IfLE(rfbEncodingH264);

    hdr.length = Swap32IfLE((uint32_t)encodedSize);
    hdr.flags = Swap32IfLE(forceKeyframe ? rfbH264FlagResetContext : rfbH264FlagNone);

    cl->h264ForceKeyframe = FALSE;

    if (cl->ublen + sz_rfbFramebufferUpdateRectHeader + sz_rfbH264Header > UPDATE_BUF_SIZE) {
        if (!rfbSendUpdateBuf(cl)) {
            return FALSE;
        }
    }

    memcpy(&cl->updateBuf[cl->ublen], &rect, sz_rfbFramebufferUpdateRectHeader);
    cl->ublen += sz_rfbFramebufferUpdateRectHeader;
    memcpy(&cl->updateBuf[cl->ublen], &hdr, sz_rfbH264Header);
    cl->ublen += sz_rfbH264Header;

    if (!rfbSendUpdateBuf(cl)) {
        return FALSE;
    }

    if (rfbWriteExact(cl, (char *)cl->h264EncodeBuffer, (int)encodedSize) < 0) {
        rfbLogPerror(H264_LOG_PREFIX "write");
        rfbCloseClient(cl);
        return FALSE;
    }

    rawEquivalent = width * height * (cl->format.bitsPerPixel / 8);
    rfbStatRecordEncodingSent(cl, rfbEncodingH264,
                              sz_rfbFramebufferUpdateRectHeader + sz_rfbH264Header + (int)encodedSize,
                              sz_rfbFramebufferUpdateRectHeader + sz_rfbH264Header + rawEquivalent);

    return TRUE;
}

#endif /* LIBVNCSERVER_HAVE_LIBAVCODEC */
