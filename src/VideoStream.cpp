// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// This file is roughly based on grd-rdp-graphics-pipeline.c from Gnome Remote
// Desktop which is:
//
// SPDX-FileCopyrightText: 2021 Pascal Nowack
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoStream.h"

#include <algorithm>
#include <condition_variable>

#include <QDateTime>
#include <QQueue>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "NetworkDetection.h"
#include "PeerContext_p.h"
#include "RdpConnection.h"

#include "krdp_logging.h"

namespace KRdp
{

namespace clk = std::chrono;

// Maximum number of frames to contain in the queue.
constexpr clk::system_clock::duration FrameRateEstimateAveragePeriod = clk::seconds(1);

struct RdpCapsInformation {
    uint32_t version;
    RDPGFX_CAPSET capSet;
    bool avcSupported : 1 = false;
    bool yuv420Supported : 1 = false;
};

const char *capVersionToString(uint32_t version)
{
    switch (version) {
    case RDPGFX_CAPVERSION_107:
        return "RDPGFX_CAPVERSION_107";
    case RDPGFX_CAPVERSION_106:
        return "RDPGFX_CAPVERSION_106";
    case RDPGFX_CAPVERSION_105:
        return "RDPGFX_CAPVERSION_105";
    case RDPGFX_CAPVERSION_104:
        return "RDPGFX_CAPVERSION_104";
    case RDPGFX_CAPVERSION_103:
        return "RDPGFX_CAPVERSION_103";
    case RDPGFX_CAPVERSION_102:
        return "RDPGFX_CAPVERSION_102";
    case RDPGFX_CAPVERSION_101:
        return "RDPGFX_CAPVERSION_101";
    case RDPGFX_CAPVERSION_10:
        return "RDPGFX_CAPVERSION_10";
    case RDPGFX_CAPVERSION_81:
        return "RDPGFX_CAPVERSION_81";
    case RDPGFX_CAPVERSION_8:
        return "RDPGFX_CAPVERSION_8";
    default:
        return "UNKNOWN_VERSION";
    }
}

BOOL gfxChannelIdAssigned(RdpgfxServerContext *context, uint32_t channelId)
{
    auto stream = reinterpret_cast<VideoStream *>(context->custom);
    if (stream->onChannelIdAssigned(channelId)) {
        return TRUE;
    }
    return FALSE;
}

uint32_t gfxCapsAdvertise(RdpgfxServerContext *context, const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise)
{
    auto stream = reinterpret_cast<VideoStream *>(context->custom);
    return stream->onCapsAdvertise(capsAdvertise);
}

uint32_t gfxFrameAcknowledge(RdpgfxServerContext *context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge)
{
    auto stream = reinterpret_cast<VideoStream *>(context->custom);
    return stream->onFrameAcknowledge(frameAcknowledge);
}

uint32_t gfxQoEFrameAcknowledge(RdpgfxServerContext *, const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU *)
{
    return CHANNEL_RC_OK;
}

struct Surface {
    uint16_t id;
    QSize size;
};

struct FrameRateEstimate {
    clk::system_clock::time_point timeStamp;
    int estimate = 0;
};

class KRDP_NO_EXPORT VideoStream::Private
{
public:
    using RdpGfxContextPtr = std::unique_ptr<RdpgfxServerContext, decltype(&rdpgfx_server_context_free)>;

    RdpConnection *session;

    RdpGfxContextPtr gfxContext = RdpGfxContextPtr(nullptr, rdpgfx_server_context_free);

    uint32_t frameId = 0;
    uint32_t channelId = 0;

    uint16_t nextSurfaceId = 1;
    Surface surface;

    bool pendingReset = true;
    bool enabled = false;
    bool capsConfirmed = false;

    /** When true, client did not advertise AVC; we decode H.264 to raw and send as UNCOMPRESSED (e.g. iPad). */
    bool useUncompressedFallback = false;
    /** Resolution scale for fallback (1=full, 2=half, 4=quarter). Used only when useUncompressedFallback. */
    int fallbackScale = 1;

    std::jthread frameSubmissionThread;
    std::mutex frameQueueMutex;

    QQueue<VideoFrame> frameQueue;
    QSet<uint32_t> pendingFrames;

    int maximumFrameRate = 120;
    int requestedFrameRate = 60;
    QQueue<FrameRateEstimate> frameRateEstimates;
    clk::system_clock::time_point lastFrameRateEstimation;

    std::atomic_int encodedFrames = 0;
    std::atomic_int frameDelay = 0;

    // H.264 decode path for uncompressed fallback (clients that don't advertise AVC)
    AVCodecContext *decCtx = nullptr;
    AVFrame *decFrame = nullptr;
    SwsContext *swsCtx = nullptr;
    int decWidth = 0;
    int decHeight = 0;
    int swsOutW = 0;
    int swsOutH = 0;
    QByteArray bgrxBuffer;
    /** Copy of decoded frame for UNCOMPRESSED send (avoids use-after-free if FreeRDP uses data pointer asynchronously). */
    QByteArray uncompressedSendBuffer;

    // Stable storage for AVC420 surface command (avoids use-after-free if FreeRDP uses pointers asynchronously)
    RDPGFX_AVC420_BITMAP_STREAM avcStream = {};
    RECTANGLE_16 avcRegionRect = {};
    RDPGFX_H264_QUANT_QUALITY avcQuality = {};
    QByteArray avcFrameData;
};

void closeUncompressedDecoder(VideoStream::Private *d)
{
    if (d->swsCtx) {
        sws_freeContext(d->swsCtx);
        d->swsCtx = nullptr;
    }
    d->swsOutW = 0;
    d->swsOutH = 0;
    if (d->decFrame) {
        av_frame_free(&d->decFrame);
        d->decFrame = nullptr;
    }
    if (d->decCtx) {
        avcodec_free_context(&d->decCtx);
        d->decCtx = nullptr;
    }
    d->decWidth = 0;
    d->decHeight = 0;
    d->bgrxBuffer.clear();
}

bool ensureUncompressedDecoder(VideoStream::Private *d, int width, int height)
{
    if (d->decCtx && d->decWidth == width && d->decHeight == height) {
        return true;
    }
    closeUncompressedDecoder(d);

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        qCWarning(KRDP) << "H.264 decoder not found (libavcodec)";
        return false;
    }
    d->decCtx = avcodec_alloc_context3(codec);
    if (!d->decCtx) {
        return false;
    }
    if (avcodec_open2(d->decCtx, codec, nullptr) < 0) {
        avcodec_free_context(&d->decCtx);
        d->decCtx = nullptr;
        qCWarning(KRDP) << "Failed to open H.264 decoder";
        return false;
    }
    d->decFrame = av_frame_alloc();
    if (!d->decFrame) {
        closeUncompressedDecoder(d);
        return false;
    }
    d->decWidth = width;
    d->decHeight = height;
    return true;
}

/** Decode H.264 frame to BGRX in d->bgrxBuffer. Returns true on success. */
bool decodeFrameToBgrx(VideoStream::Private *d, const VideoFrame &frame)
{
    if (!ensureUncompressedDecoder(d, frame.size.width(), frame.size.height())) {
        return false;
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        return false;
    }
    pkt->data = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(frame.data.constData()));
    pkt->size = frame.data.size();

    int ret = avcodec_send_packet(d->decCtx, pkt);
    av_packet_free(&pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        return false;
    }

    ret = avcodec_receive_frame(d->decCtx, d->decFrame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return false;
    }
    if (ret < 0) {
        return false;
    }

    const int w = d->decFrame->width;
    const int h = d->decFrame->height;
    const int scale = d->useUncompressedFallback ? d->fallbackScale : 1;
    const int outW = (scale > 1) ? (w / scale) : w;
    const int outH = (scale > 1) ? (h / scale) : h;
    const size_t bgrxSize = static_cast<size_t>(outW) * outH * 4;
    d->bgrxBuffer.resize(static_cast<int>(bgrxSize));

    if (!d->swsCtx || d->swsOutW != outW || d->swsOutH != outH) {
        if (d->swsCtx) {
            sws_freeContext(d->swsCtx);
            d->swsCtx = nullptr;
        }
        d->swsCtx = sws_getContext(w, h, static_cast<AVPixelFormat>(d->decFrame->format),
                                   outW, outH, AV_PIX_FMT_BGRA,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
        d->swsOutW = outW;
        d->swsOutH = outH;
    }
    if (!d->swsCtx) {
        return false;
    }

    uint8_t *dst[1] = { reinterpret_cast<uint8_t *>(d->bgrxBuffer.data()) };
    int dstStride[1] = { outW * 4 };
    sws_scale(d->swsCtx, d->decFrame->data, d->decFrame->linesize, 0, h, dst, dstStride);
    return true;
}

VideoStream::VideoStream(RdpConnection *session)
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
    d->session = session;
}

VideoStream::~VideoStream()
{
}

bool VideoStream::initialize()
{
    if (d->gfxContext) {
        return true;
    }

    auto peerContext = reinterpret_cast<PeerContext *>(d->session->rdpPeerContext());

    d->gfxContext = Private::RdpGfxContextPtr{rdpgfx_server_context_new(peerContext->virtualChannelManager), rdpgfx_server_context_free};
    if (!d->gfxContext) {
        qCWarning(KRDP) << "Failed creating RDPGFX context";
        return false;
    }

    d->gfxContext->ChannelIdAssigned = gfxChannelIdAssigned;
    d->gfxContext->CapsAdvertise = gfxCapsAdvertise;
    d->gfxContext->FrameAcknowledge = gfxFrameAcknowledge;
    d->gfxContext->QoeFrameAcknowledge = gfxQoEFrameAcknowledge;

    d->gfxContext->custom = this;
    d->gfxContext->rdpcontext = d->session->rdpPeerContext();

    if (!d->gfxContext->Open(d->gfxContext.get())) {
        qCWarning(KRDP) << "Could not open GFX context";
        return false;
    }

    connect(d->session->networkDetection(), &NetworkDetection::rttChanged, this, &VideoStream::updateRequestedFrameRate);

    d->frameSubmissionThread = std::jthread([this](std::stop_token token) {
        while (!token.stop_requested()) {
            VideoFrame nextFrame;
            {
                std::unique_lock lock(d->frameQueueMutex);
                if (!d->frameQueue.isEmpty()) {
                    nextFrame = d->frameQueue.takeFirst();
                }
            }
            if (nextFrame.size.isEmpty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000) / d->requestedFrameRate);
                continue;
            }
            sendFrame(nextFrame);
            if (d->useUncompressedFallback) {
                const int maxFps = d->session->fallbackMaxFrameRate();
                if (maxFps > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000) / maxFps);
                }
            }
        }
    });

    qCDebug(KRDP) << "Video stream initialized";

    return true;
}

void VideoStream::close()
{
    if (!d->gfxContext) {
        return;
    }

    closeUncompressedDecoder(d.get());

    d->gfxContext->Close(d->gfxContext.get());

    if (d->frameSubmissionThread.joinable()) {
        d->frameSubmissionThread.request_stop();
        d->frameSubmissionThread.join();
    }

    Q_EMIT closed();
}

void VideoStream::queueFrame(const KRdp::VideoFrame &frame)
{
    if (d->session->state() != RdpConnection::State::Streaming || !d->enabled) {
        return;
    }

    std::lock_guard lock(d->frameQueueMutex);
    d->frameQueue.append(frame);
}

void VideoStream::reset()
{
    d->pendingReset = true;
}

bool VideoStream::enabled() const
{
    return d->enabled;
}

void VideoStream::setEnabled(bool enabled)
{
    if (d->enabled == enabled) {
        return;
    }

    d->enabled = enabled;
    Q_EMIT enabledChanged();
}

uint32_t VideoStream::requestedFrameRate() const
{
    return d->requestedFrameRate;
}

bool VideoStream::onChannelIdAssigned(uint32_t channelId)
{
    d->channelId = channelId;

    return true;
}

uint32_t VideoStream::onCapsAdvertise(const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise)
{
    auto capsSets = capsAdvertise->capsSets;
    auto count = capsAdvertise->capsSetCount;

    std::vector<RdpCapsInformation> capsInformation;
    capsInformation.reserve(count);

    qCDebug(KRDP) << "Received caps:";
    for (int i = 0; i < count; ++i) {
        auto set = capsSets[i];

        RdpCapsInformation caps;
        caps.version = set.version;
        caps.capSet = set;

        switch (set.version) {
        case RDPGFX_CAPVERSION_107:
        case RDPGFX_CAPVERSION_106:
        case RDPGFX_CAPVERSION_105:
        case RDPGFX_CAPVERSION_104:
            caps.yuv420Supported = true;
            Q_FALLTHROUGH();
        case RDPGFX_CAPVERSION_103:
        case RDPGFX_CAPVERSION_102:
        case RDPGFX_CAPVERSION_101:
        case RDPGFX_CAPVERSION_10:
            if (!(set.flags & RDPGFX_CAPS_FLAG_AVC_DISABLED)) {
                caps.avcSupported = true;
            }
            break;
        case RDPGFX_CAPVERSION_81:
            if (set.flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) {
                caps.avcSupported = true;
                caps.yuv420Supported = true;
            }
            break;
        case RDPGFX_CAPVERSION_8:
            break;
        }

        qCDebug(KRDP) << " " << capVersionToString(caps.version) << "AVC:" << caps.avcSupported << "YUV420:" << caps.yuv420Supported;

        capsInformation.push_back(caps);
    }

    // Prefer a cap set that advertises both AVC and YUV420 when available.
    const bool hasAvcYuv420 = std::any_of(capsInformation.begin(), capsInformation.end(), [](const RdpCapsInformation &caps) {
        return caps.avcSupported && caps.yuv420Supported;
    });
    const bool hasYuv420 = std::any_of(capsInformation.begin(), capsInformation.end(), [](const RdpCapsInformation &caps) {
        return caps.yuv420Supported;
    });

    if (!hasYuv420) {
        qCWarning(KRDP) << "Client does not support YUV420. Try another RDP client (e.g. FreeRDP, Remmina, Windows RDP).";
        d->session->close(RdpConnection::CloseReason::VideoInitFailed);
        return CHANNEL_RC_INITIALIZATION_ERROR;
    }

    // Choose best cap set: prefer AVC+YUV420, then highest version with at least YUV420.
    auto maxVersion = std::max_element(capsInformation.begin(), capsInformation.end(), [hasAvcYuv420](const auto &first, const auto &second) {
        if (!first.yuv420Supported) {
            return true;
        }
        if (!second.yuv420Supported) {
            return false;
        }
        bool firstPreferred = hasAvcYuv420 && (first.avcSupported && first.yuv420Supported);
        bool secondPreferred = hasAvcYuv420 && (second.avcSupported && second.yuv420Supported);
        if (firstPreferred != secondPreferred) {
            return secondPreferred;
        }
        return first.version < second.version;
    });

    if (!maxVersion->yuv420Supported) {
        qCWarning(KRDP) << "No YUV420 cap set found.";
        d->session->close(RdpConnection::CloseReason::VideoInitFailed);
        return CHANNEL_RC_INITIALIZATION_ERROR;
    }

    // Use AVC420 only when the selected cap set explicitly advertises AVC. Otherwise use
    // uncompressed fallback so we don't send H.264 to clients that don't support it.
    const bool useAvc = maxVersion->avcSupported && maxVersion->yuv420Supported;
    if (!useAvc) {
        d->useUncompressedFallback = true;
        d->fallbackScale = std::max(1, d->session->fallbackScale());
        d->session->setClientToHostScale(static_cast<qreal>(d->fallbackScale));
        d->requestedFrameRate = std::min(d->requestedFrameRate, d->session->fallbackMaxFrameRate());
        qCDebug(KRDP) << "Selected cap set does not advertise AVC; using uncompressed RDPGFX fallback (scale" << d->fallbackScale << ", max" << d->session->fallbackMaxFrameRate() << "fps).";
    } else {
        d->session->setClientToHostScale(1.0);
        qCDebug(KRDP) << "Using H.264 (AVC420) for this session.";
    }
    qCDebug(KRDP) << "Selected caps:" << capVersionToString(maxVersion->version);

    RDPGFX_CAPS_CONFIRM_PDU capsConfirmPdu;
    capsConfirmPdu.capsSet = &(maxVersion->capSet);
    d->gfxContext->CapsConfirm(d->gfxContext.get(), &capsConfirmPdu);

    d->capsConfirmed = true;

    return CHANNEL_RC_OK;
}

uint32_t VideoStream::onFrameAcknowledge(const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge)
{
    auto id = frameAcknowledge->frameId;

    auto itr = d->pendingFrames.constFind(id);
    if (itr == d->pendingFrames.cend()) {
        qCWarning(KRDP) << "Got frame acknowledge for an unknown frame";
        return CHANNEL_RC_OK;
    }

    if (frameAcknowledge->queueDepth & SUSPEND_FRAME_ACKNOWLEDGEMENT) {
        // Client requested suspend; no log to avoid log spam
    }

    d->frameDelay = d->encodedFrames - frameAcknowledge->totalFramesDecoded;
    d->pendingFrames.erase(itr);

    return CHANNEL_RC_OK;
}

void VideoStream::performReset(QSize size)
{
    const int scale = d->useUncompressedFallback ? d->fallbackScale : 1;
    const QSize gfxSize = (scale > 1) ? QSize(size.width() / scale, size.height() / scale) : size;

    RDPGFX_RESET_GRAPHICS_PDU resetGraphicsPdu;
    resetGraphicsPdu.width = gfxSize.width();
    resetGraphicsPdu.height = gfxSize.height();
    resetGraphicsPdu.monitorCount = 1;

    auto monitors = new MONITOR_DEF[1];
    monitors[0].left = 0;
    monitors[0].right = gfxSize.width();
    monitors[0].top = 0;
    monitors[0].bottom = gfxSize.height();
    monitors[0].flags = MONITOR_PRIMARY;
    resetGraphicsPdu.monitorDefArray = monitors;
    d->gfxContext->ResetGraphics(d->gfxContext.get(), &resetGraphicsPdu);

    RDPGFX_CREATE_SURFACE_PDU createSurfacePdu;
    createSurfacePdu.width = gfxSize.width();
    createSurfacePdu.height = gfxSize.height();
    uint16_t surfaceId = d->nextSurfaceId++;
    createSurfacePdu.surfaceId = surfaceId;
    createSurfacePdu.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;
    d->gfxContext->CreateSurface(d->gfxContext.get(), &createSurfacePdu);

    d->surface = Surface{
        .id = surfaceId,
        .size = gfxSize,
    };

    RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU mapSurfaceToOutputPdu;
    mapSurfaceToOutputPdu.outputOriginX = 0;
    mapSurfaceToOutputPdu.outputOriginY = 0;
    mapSurfaceToOutputPdu.surfaceId = surfaceId;
    d->gfxContext->MapSurfaceToOutput(d->gfxContext.get(), &mapSurfaceToOutputPdu);
}

void VideoStream::sendFrame(const VideoFrame &frame)
{
    if (!d->gfxContext || !d->capsConfirmed) {
        return;
    }

    if (frame.data.size() == 0) {
        return;
    }

    if (d->pendingReset) {
        d->pendingReset = false;
        performReset(frame.size);
    }

    d->session->networkDetection()->startBandwidthMeasure();

    auto frameId = d->frameId++;

    d->encodedFrames++;

    d->pendingFrames.insert(frameId);

    RDPGFX_START_FRAME_PDU startFramePdu;
    RDPGFX_END_FRAME_PDU endFramePdu;

    auto now = QDateTime::currentDateTimeUtc().time();
    startFramePdu.timestamp = now.hour() << 22 | now.minute() << 16 | now.second() << 10 | now.msec();

    startFramePdu.frameId = frameId;
    endFramePdu.frameId = frameId;

    const int scale = d->useUncompressedFallback ? d->fallbackScale : 1;
    const int cmdWidth = (scale > 1) ? (frame.size.width() / scale) : frame.size.width();
    const int cmdHeight = (scale > 1) ? (frame.size.height() / scale) : frame.size.height();

    RDPGFX_SURFACE_COMMAND surfaceCommand;
    surfaceCommand.surfaceId = d->surface.id;
    surfaceCommand.format = PIXEL_FORMAT_BGRX32;
    surfaceCommand.left = 0;
    surfaceCommand.top = 0;
    surfaceCommand.right = cmdWidth;
    surfaceCommand.bottom = cmdHeight;

    if (d->useUncompressedFallback) {
        if (!decodeFrameToBgrx(d.get(), frame)) {
            d->session->networkDetection()->stopBandwidthMeasure();
            return;
        }
        d->uncompressedSendBuffer = d->bgrxBuffer;
        surfaceCommand.codecId = RDPGFX_CODECID_UNCOMPRESSED;
        surfaceCommand.length = d->uncompressedSendBuffer.size();
        surfaceCommand.data = reinterpret_cast<BYTE *>(d->uncompressedSendBuffer.data());
        surfaceCommand.extra = nullptr;
    } else {
        surfaceCommand.codecId = RDPGFX_CODECID_AVC420;
        surfaceCommand.length = 0;
        surfaceCommand.data = nullptr;

        d->avcFrameData = frame.data;
        d->avcStream = {};
        d->avcStream.data = reinterpret_cast<BYTE *>(d->avcFrameData.data());
        d->avcStream.length = d->avcFrameData.size();
        d->avcStream.meta.numRegionRects = 1;
        d->avcRegionRect.left = 0;
        d->avcRegionRect.top = 0;
        d->avcRegionRect.right = frame.size.width();
        d->avcRegionRect.bottom = frame.size.height();
        d->avcStream.meta.regionRects = &d->avcRegionRect;
        d->avcQuality.qp = 22;
        d->avcQuality.p = 0;
        d->avcQuality.qualityVal = 100;
        d->avcStream.meta.quantQualityVals = &d->avcQuality;
        surfaceCommand.extra = &d->avcStream;
    }

    d->gfxContext->StartFrame(d->gfxContext.get(), &startFramePdu);
    d->gfxContext->SurfaceCommand(d->gfxContext.get(), &surfaceCommand);

    d->gfxContext->EndFrame(d->gfxContext.get(), &endFramePdu);

    d->session->networkDetection()->stopBandwidthMeasure();
}

void VideoStream::updateRequestedFrameRate()
{
    auto rtt = std::max(clk::duration_cast<clk::milliseconds>(d->session->networkDetection()->averageRTT()), clk::milliseconds(1));
    auto now = clk::system_clock::now();

    FrameRateEstimate estimate;
    estimate.timeStamp = now;
    estimate.estimate = std::min(int(clk::milliseconds(1000) / (rtt * std::max(d->frameDelay.load(), 1))), d->maximumFrameRate);
    d->frameRateEstimates.append(estimate);

    if (now - d->lastFrameRateEstimation < FrameRateEstimateAveragePeriod) {
        return;
    }

    d->lastFrameRateEstimation = now;

    d->frameRateEstimates.erase(std::remove_if(d->frameRateEstimates.begin(),
                                               d->frameRateEstimates.end(),
                                               [now](const auto &estimate) {
                                                   return (estimate.timeStamp - now) > FrameRateEstimateAveragePeriod;
                                               }),
                                d->frameRateEstimates.cend());

    auto sum = std::accumulate(d->frameRateEstimates.cbegin(), d->frameRateEstimates.cend(), 0, [](int acc, const auto &estimate) {
        return acc + estimate.estimate;
    });
    auto average = sum / d->frameRateEstimates.size();

    // we want some headroom so we can always clear our current load
    // and handle any other latency
    constexpr qreal targetFrameRateSaturation = 0.5;
    auto frameRate = std::max(1.0, average * targetFrameRateSaturation);

    if (d->useUncompressedFallback) {
        frameRate = std::min(frameRate, static_cast<double>(d->session->fallbackMaxFrameRate()));
    }
    if (frameRate != d->requestedFrameRate) {
        d->requestedFrameRate = frameRate;
        Q_EMIT requestedFrameRateChanged();
    }
}
}

#include "moc_VideoStream.cpp"
