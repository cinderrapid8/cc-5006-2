#pragma once

#include "core/Types.h"
#include <QImage>
#include <memory>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

struct SwsContext;

namespace sr {

struct DecodedFrame {
    QImage image;          // RGB24
    int index = -1;        // unique source frame number
    int64_t ptsUs = 0;     // real presentation time
    int64_t durationUs = 0;
    bool keyFrame = false;
};

// Decodes a source file and exposes:
//  - a complete frame index with real (non-uniform) timestamps,
//  - exact per-frame seeking (seeks to a decodable keyframe, then decodes
//    through to the requested frame; keyframes are never returned as
//    approximations of the requested frame),
//  - truncated-tail detection.
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    bool open(const QString& path, QString* err = nullptr);
    void close();

    bool isOpen() const;
    const SourceSummary& summary() const { return m_summary; }
    const QVector<FrameTime>& index() const { return m_index; }
    int frameCount() const { return m_index.size(); }

    // Full sequential pass: builds the index of real timestamps.
    // progress(reportFrame, approxTotalFrames) may be null.
    bool buildIndex(std::function<void(int, int)> progress = nullptr,
                    QString* err = nullptr);

    // Exact positioning. After this, readNextFrame() returns frame targetIndex.
    // Internally: backward seek to the preceding keyframe, then decode and
    // discard every frame until targetIndex is reached.
    bool seekToFrame(int targetIndex, QString* err = nullptr);

    // Sequential read from current position. Returns false at end of stream
    // or on a hard decode error (truncated tail). The frame index always
    // matches its position in the built index.
    bool readNextFrame(DecodedFrame& out, QString* err = nullptr);

    // Convenience: seek + first read.
    bool frameAt(int targetIndex, DecodedFrame& out, QString* err = nullptr);

    AVFormatContext* formatCtx() const { return m_fmt; }
    AVCodecContext* codecCtx() const { return m_codec; }

private:
    bool openStreams(QString* err);
    bool drainDecoder(AVPacket* pkt, bool* hardError);
    void convertFrame(const AVFrame* av, QImage& img);
    int64_t ptsToUs(int64_t pts) const;

    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_codec = nullptr;
    ::SwsContext* m_sws = nullptr;
    int m_streamIdx = -1;
    int m_audioIdx = -1;
    AVRational m_timebase{0, 1};

    SourceSummary m_summary;
    QVector<FrameTime> m_index;

    int m_curIndex = -1;        // index of last frame returned by readNextFrame
    bool m_eof = false;
    bool m_hardError = false;
    int m_decodeErrors = 0;
    QImage m_seekCacheImage;
    bool m_seekCacheValid = false;
};

// File digest used to bind markers to a concrete source file.
QString sha256OfFile(const QString& path, QString* err = nullptr);

} // namespace sr
