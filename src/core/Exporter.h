#pragma once

#include "core/Types.h"
#include "core/VideoDecoder.h"
#include <QJsonObject>
#include <QVector>

namespace sr {

enum class SlowmoMode {
    Repeat = 0,      // explicit frame repetition, no synthesis
    Interpolate = 1, // linear blend between adjacent source frames
};

enum class AudioMode {
    Mute = 0,        // no audio stream in the output
    Drop = 1,        // resample: pitch shifts down with speed, pitch NOT preserved
    Atempo = 2,      // libavfilter atempo: tempo changed, pitch preserved
};

struct ExportSettings {
    QString outputPath;
    int startFrame = 0;          // inclusive, unique source frame number
    int endFrame = 0;            // inclusive
    double speed = 0.5;          // 0.5 -> 2x duration
    SlowmoMode slowmoMode = SlowmoMode::Repeat;
    AudioMode audioMode = AudioMode::Mute;
    int outputFrameRate = 60;    // fixed output clock, up to 120
};

struct FrameProvenance {
    int outputIndex = 0;
    int64_t outputPtsUs = 0;
    bool generated = true;       // true = produced by repeat/interpolation
    int sourceFrameA = -1;
    int sourceFrameB = -1;       // -1 in repeat/original case
    double blendWeight = 0.0;    // weight of B in an interpolated frame
    int64_t sourceTimeUs = 0;    // source timeline position represented
    QString kind;                // "original" | "repeat" | "interpolated"
};

struct ExportReport {
    bool ok = false;
    QString error;
    QString outputPath;
    QString sidecarPath;
    int outputFrameCount = 0;
    int originalFrameCount = 0;
    int generatedFrameCount = 0;
    int64_t outputDurationUs = 0;
    int64_t expectedDurationUs = 0;
    int64_t sourceSpanUs = 0;
    int startFrame = -1;
    int endFrame = -1;
    bool hasAudio = false;
    AudioMode audioMode = AudioMode::Mute;
    QVector<FrameProvenance> frames;
};

class Exporter {
public:
    explicit Exporter(VideoDecoder& decoder);

    bool run(const ExportSettings& s, ExportReport& report,
             std::function<void(int, int)> progress = nullptr);

    static QString slowmoModeName(SlowmoMode m);
    static QString audioModeName(AudioMode m);

private:
    VideoDecoder& m_dec;
};

} // namespace sr
