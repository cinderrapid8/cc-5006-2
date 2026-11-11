#include "core/VideoDecoder.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace sr {

static QString avErr(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

QString sha256OfFile(const QString& path, QString* err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("cannot open %1: %2").arg(path, f.errorString());
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) {
        if (err) *err = QStringLiteral("failed reading %1").arg(path);
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::isOpen() const { return m_fmt != nullptr; }

bool VideoDecoder::open(const QString& path, QString* err)
{
    close();
    int rc = avformat_open_input(&m_fmt, path.toUtf8().constData(), nullptr, nullptr);
    if (rc < 0) {
        if (err) *err = QStringLiteral("avformat_open_input failed: %1").arg(avErr(rc));
        m_fmt = nullptr;
        return false;
    }
    rc = avformat_find_stream_info(m_fmt, nullptr);
    if (rc < 0) {
        if (err) *err = QStringLiteral("avformat_find_stream_info failed: %1").arg(avErr(rc));
        close();
        return false;
    }
    if (!openStreams(err)) { close(); return false; }

    QFileInfo fi(path);
    m_summary.path = fi.absoluteFilePath();
    m_summary.sizeBytes = fi.size();
    m_summary.mtimeSec = fi.lastModified().toSecsSinceEpoch();
    m_summary.sha256 = sha256OfFile(path);
    m_summary.width = m_codec->width;
    m_summary.height = m_codec->height;
    m_summary.videoStreamIndex = m_streamIdx;
    m_summary.audioStreamIndex = m_audioIdx;
    const AVStream* st = m_fmt->streams[m_streamIdx];
    if (st->avg_frame_rate.den && st->avg_frame_rate.num)
        m_summary.avgFrameRate = av_q2d(st->avg_frame_rate);
    if (st->duration != AV_NOPTS_VALUE)
        m_summary.durationUs = ptsToUs(st->duration);
    else if (m_fmt->duration != AV_NOPTS_VALUE)
        m_summary.durationUs = m_fmt->duration; // AV_TIME_BASE is already us
    return true;
}

bool VideoDecoder::openStreams(QString* err)
{
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        AVStream* st = m_fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && m_streamIdx < 0)
            m_streamIdx = static_cast<int>(i);
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && m_audioIdx < 0)
            m_audioIdx = static_cast<int>(i);
    }
    if (m_streamIdx < 0) {
        if (err) *err = QStringLiteral("no video stream");
        return false;
    }
    AVStream* st = m_fmt->streams[m_streamIdx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        if (err) *err = QStringLiteral("decoder not found for codec id %1").arg(st->codecpar->codec_id);
        return false;
    }
    m_codec = avcodec_alloc_context3(codec);
    if (!m_codec) { if (err) *err = QStringLiteral("avcodec_alloc_context3 failed"); return false; }
    int rc = avcodec_parameters_to_context(m_codec, st->codecpar);
    if (rc < 0) { if (err) *err = avErr(rc); return false; }
    m_codec->pkt_timebase = st->time_base;
    rc = avcodec_open2(m_codec, codec, nullptr);
    if (rc < 0) { if (err) *err = QStringLiteral("avcodec_open2: %1").arg(avErr(rc)); return false; }
    m_timebase = st->time_base;
    return true;
}

void VideoDecoder::close()
{
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_codec) { avcodec_free_context(&m_codec); }
    if (m_fmt) { avformat_close_input(&m_fmt); }
    m_streamIdx = -1;
    m_audioIdx = -1;
    m_index.clear();
    m_curIndex = -1;
    m_eof = false;
    m_hardError = false;
    m_decodeErrors = 0;
    m_summary = SourceSummary{};
}

int64_t VideoDecoder::ptsToUs(int64_t pts) const
{
    if (pts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    return av_rescale_q(pts, m_timebase, AVRational{1, 1'000'000});
}

bool VideoDecoder::buildIndex(std::function<void(int, int)> progress, QString* err)
{
    if (!m_fmt) { if (err) *err = QStringLiteral("decoder not open"); return false; }
    m_index.clear();
    m_decodeErrors = 0;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int64_t fallbackStepUs = m_summary.avgFrameRate > 0
        ? static_cast<int64_t>(1'000'000.0 / m_summary.avgFrameRate) : 40000;
    int64_t lastPtsUs = 0;
    bool readError = false;

    while (true) {
        int rc = av_read_frame(m_fmt, pkt);
        if (rc == AVERROR_EOF) break;
        if (rc < 0) {
            // Read failure before clean EOF: file tail is damaged/truncated.
            readError = true;
            ++m_decodeErrors;
            break;
        }
        if (pkt->stream_index != m_streamIdx) { av_packet_unref(pkt); continue; }

        rc = avcodec_send_packet(m_codec, pkt);
        av_packet_unref(pkt);
        if (rc == AVERROR_INVALIDDATA) { ++m_decodeErrors; continue; }
        if (rc < 0 && rc != AVERROR(EAGAIN)) { readError = true; ++m_decodeErrors; break; }

        while (true) {
            rc = avcodec_receive_frame(m_codec, frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc == AVERROR_INVALIDDATA) { ++m_decodeErrors; continue; }
            if (rc < 0) { readError = true; ++m_decodeErrors; break; }

            int64_t ptsUs = ptsToUs(frame->pts);
            if (ptsUs == AV_NOPTS_VALUE)
                ptsUs = lastPtsUs + fallbackStepUs;
            FrameTime ft;
            ft.index = m_index.size();
            ft.ptsUs = ptsUs;
            ft.durationUs = 0; // filled in a second pass below
            m_index.push_back(ft);
            lastPtsUs = ptsUs;
            if (progress) progress(m_index.size(), 0);
            av_frame_unref(frame);
        }
        if (readError) break;
    }
    // Flush buffered frames.
    avcodec_send_packet(m_codec, nullptr);
    while (true) {
        int rc = avcodec_receive_frame(m_codec, frame);
        if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) break;
        if (rc == AVERROR_INVALIDDATA) { ++m_decodeErrors; continue; }
        if (rc < 0) { ++m_decodeErrors; readError = true; break; }
        int64_t ptsUs = ptsToUs(frame->pts);
        if (ptsUs == AV_NOPTS_VALUE) ptsUs = lastPtsUs + fallbackStepUs;
        FrameTime ft;
        ft.index = m_index.size();
        ft.ptsUs = ptsUs;
        ft.durationUs = 0;
        m_index.push_back(ft);
        lastPtsUs = ptsUs;
        if (progress) progress(m_index.size(), 0);
        av_frame_unref(frame);
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);

    // Non-uniform durations come from the actual neighbour timestamps;
    // frame rate is never assumed constant.
    for (int i = 0; i < m_index.size(); ++i) {
        if (i + 1 < m_index.size())
            m_index[i].durationUs = m_index[i + 1].ptsUs - m_index[i].ptsUs;
        else
            m_index[i].durationUs = m_index.size() > 1
                ? m_index[i - 1].durationUs : fallbackStepUs;
        if (m_index[i].durationUs <= 0) m_index[i].durationUs = fallbackStepUs;
    }
    if (m_index.isEmpty()) {
        if (err) *err = QStringLiteral("no decodable video frames");
        return false;
    }
    if (readError || m_decodeErrors > 0)
        m_summary.truncatedTail = true;

    // Some containers (notably Matroska) swallow a truncated tail at the
    // demuxer level and simply return EOF, so the packet loop above sees no
    // explicit error. Detect the damage from container-level evidence:
    //  1) declared frame count exceeds what actually decoded, or
    //  2) the declared stream duration extends well past the last frame.
    // Some containers (notably Matroska) swallow a truncated tail at the
    // demuxer level and return plain EOF even when bytes were cut mid-stream.
    const AVStream* vst = m_fmt->streams[m_streamIdx];
    QVector<int64_t> durs;
    durs.reserve(m_index.size());
    for (const auto& ft : m_index) durs.push_back(ft.durationUs);
    std::sort(durs.begin(), durs.end());
    const int64_t medianUs = qMax<int64_t>(durs[durs.size() / 2], 1);
    const int64_t gotEndUs = m_index.last().ptsUs + m_index.last().durationUs;

    if (vst->nb_frames > m_index.size())
        m_summary.truncatedTail = true;
    if (vst->duration != AV_NOPTS_VALUE) {
        const int64_t streamEndUs = ptsToUs(vst->duration);
        if (streamEndUs - gotEndUs > medianUs)
            m_summary.truncatedTail = true;
    }
    if (m_fmt->duration != AV_NOPTS_VALUE
        && m_fmt->duration - gotEndUs > medianUs) {
        m_summary.truncatedTail = true;
    }

    if (m_summary.durationUs <= 0)
        m_summary.durationUs = m_index.last().ptsUs + m_index.last().durationUs;
    return true;
}

bool VideoDecoder::seekToFrame(int targetIndex, QString* err)
{
    if (targetIndex < 0 || targetIndex >= m_index.size()) {
        if (err) *err = QStringLiteral("frame %1 out of range [0,%2)")
                            .arg(targetIndex).arg(m_index.size());
        return false;
    }
    const int64_t targetPtsUs = m_index[targetIndex].ptsUs;
    const int64_t targetTs = av_rescale_q(targetPtsUs, AVRational{1, 1'000'000},
                                          m_timebase);
    // Enter at the nearest decodable position at or before the target.
    int rc = av_seek_frame(m_fmt, m_streamIdx, targetTs, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        if (err) *err = QStringLiteral("av_seek_frame: %1").arg(avErr(rc));
        return false;
    }
    avcodec_flush_buffers(m_codec);
    m_curIndex = -1;
    m_eof = false;
    m_hardError = false;

    // Decode forward until the frame whose real pts equals the target.
    // The keyframe landed on by seek is only an entry point, never the
    // answer to the frame request.
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool reached = false;
    bool fail = false;
    while (!reached && !fail) {
        rc = av_read_frame(m_fmt, pkt);
        if (rc == AVERROR_EOF) {
            avcodec_send_packet(m_codec, nullptr);
        } else if (rc < 0) {
            fail = true;
            if (err) *err = QStringLiteral("read after seek: %1").arg(avErr(rc));
            break;
        } else {
            if (pkt->stream_index != m_streamIdx) { av_packet_unref(pkt); continue; }
            int s = avcodec_send_packet(m_codec, pkt);
            av_packet_unref(pkt);
            if (s < 0 && s != AVERROR(EAGAIN)) { fail = true; if (err) *err = avErr(s); break; }
        }
        while (true) {
            int r = avcodec_receive_frame(m_codec, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) { fail = true; if (err) *err = avErr(r); break; }
            int64_t ptsUs = ptsToUs(frame->pts);
            if (ptsUs == targetPtsUs) {
                reached = true;
                // Keep this frame: store decoded image by re-reading via
                // readNextFrame path would lose it, so cache via curIndex
                // marker and let readNextFrame decode again would waste work.
                // Simpler: return the image through a one-frame cache.
                m_seekCacheImage = QImage();
                convertFrame(frame, m_seekCacheImage);
                m_seekCacheValid = true;
                m_curIndex = targetIndex;
            }
            av_frame_unref(frame);
            if (reached) break;
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (!reached && !fail && err)
        *err = QStringLiteral("target frame pts %1 not reached after seek").arg(targetPtsUs);
    return reached;
}

bool VideoDecoder::readNextFrame(DecodedFrame& out, QString* err)
{
    if (!m_fmt) { if (err) *err = QStringLiteral("decoder not open"); return false; }
    if (m_eof || m_hardError) return false;

    // Serve the frame reached inside seekToFrame first.
    if (m_seekCacheValid) {
        m_seekCacheValid = false;
        const FrameTime& ft = m_index[m_curIndex];
        out.index = ft.index;
        out.ptsUs = ft.ptsUs;
        out.durationUs = ft.durationUs;
        out.image = m_seekCacheImage;
        out.keyFrame = false; // informational only; cache carries a full frame
        return true;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool got = false;
    while (!got) {
        int rc = av_read_frame(m_fmt, pkt);
        if (rc == AVERROR_EOF) {
            avcodec_send_packet(m_codec, nullptr);
            while (true) {
                int r = avcodec_receive_frame(m_codec, frame);
                if (r == AVERROR_EOF || r == AVERROR(EAGAIN)) { m_eof = true; break; }
                if (r < 0) { m_hardError = true; break; }
                got = true;
                break;
            }
            if (!got) break;
        } else if (rc < 0) {
            m_hardError = true;
            if (err) *err = QStringLiteral("av_read_frame: %1").arg(avErr(rc));
            break;
        } else {
            if (pkt->stream_index != m_streamIdx) { av_packet_unref(pkt); continue; }
            int s = avcodec_send_packet(m_codec, pkt);
            av_packet_unref(pkt);
            if (s == AVERROR_INVALIDDATA) { ++m_decodeErrors; m_summary.truncatedTail = true; continue; }
            if (s < 0 && s != AVERROR(EAGAIN)) { m_hardError = true; if (err) *err = avErr(s); break; }
            int r = avcodec_receive_frame(m_codec, frame);
            if (r == AVERROR(EAGAIN)) continue;
            if (r == AVERROR_EOF) { m_eof = true; break; }
            if (r == AVERROR_INVALIDDATA) { ++m_decodeErrors; m_summary.truncatedTail = true; continue; }
            if (r < 0) { m_hardError = true; if (err) *err = avErr(r); break; }
            got = true;
        }
    }
    if (!got) {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        return false;
    }

    int next = m_curIndex + 1;
    if (next >= m_index.size()) {
        m_eof = true;
        av_frame_free(&frame);
        av_packet_free(&pkt);
        return false;
    }
    int64_t ptsUs = ptsToUs(frame->pts);
    if (ptsUs != m_index[next].ptsUs) {
        // Reorder/edge case: locate index entry by pts.
        int found = -1;
        for (int i = qMax(0, next - 2); i < m_index.size() && i <= next + 2; ++i) {
            if (m_index[i].ptsUs == ptsUs) { found = i; break; }
        }
        if (found < 0) {
            av_frame_unref(frame);
            av_frame_free(&frame);
            av_packet_free(&pkt);
            if (err) *err = QStringLiteral("decoded pts %1 does not match frame index").arg(ptsUs);
            m_hardError = true;
            return false;
        }
        next = found;
    }
    const FrameTime& ft = m_index[next];
    out.index = ft.index;
    out.ptsUs = ft.ptsUs;
    out.durationUs = ft.durationUs;
    out.keyFrame = frame->key_frame != 0;
    convertFrame(frame, out.image);
    m_curIndex = next;
    av_frame_unref(frame);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return true;
}

bool VideoDecoder::frameAt(int targetIndex, DecodedFrame& out, QString* err)
{
    if (!seekToFrame(targetIndex, err)) return false;
    return readNextFrame(out, err);
}

void VideoDecoder::convertFrame(const AVFrame* av, QImage& img)
{
    m_sws = sws_getCachedContext(m_sws, av->width, av->height,
                                 static_cast<AVPixelFormat>(av->format),
                                 av->width, av->height, AV_PIX_FMT_RGB24,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
    img = QImage(av->width, av->height, QImage::Format_RGB888);
    uint8_t* dst[4] = { img.bits(), nullptr, nullptr, nullptr };
    int dstLinesize[4] = { static_cast<int>(img.bytesPerLine()), 0, 0, 0 };
    sws_scale(m_sws, av->data, av->linesize, 0, av->height, dst, dstLinesize);
}

} // namespace sr
