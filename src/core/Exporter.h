#pragma once

#include "core/FramePlanner.h"
#include "core/SpeedCurve.h"
#include "core/VideoDecoder.h"
#include <QJsonObject>
#include <QVector>
#include <functional>

namespace sr {

enum class AudioMode {
    Mute = 0,        // no audio stream in the output
    Drop = 1,        // resample: pitch shifts down with speed, pitch NOT preserved
    Atempo = 2,      // libavfilter atempo: tempo changed, pitch preserved
};

struct ExportSettings {
    QString outputPath;
    int startFrame = 0;          // inclusive, unique source frame number
    int endFrame = 0;            // inclusive

    // Primary timing definition. When hasCurve is true the exporter uses this
    // mapping for both the preview-cached frame selection and the render.
    // When false the legacy constant `speed` is wrapped into a one-segment
    // SpeedCurve (the historical constant slow-motion special case).
    bool hasCurve = false;
    SpeedCurve curve;
    double speed = 0.5;          // legacy constant speed, 0.5 -> 2x duration

    SlowmoMode slowmoMode = SlowmoMode::Repeat;
    AudioMode audioMode = AudioMode::Mute;
    AudioFreezePolicy audioFreezePolicy = AudioFreezePolicy::Silence;
    int outputFrameRate = 60;    // fixed output clock, up to 120

    // Effective mapping regardless of which field populated it.
    SpeedCurve effectiveCurve(int64_t spanUs) const
    {
        if (hasCurve) return curve;
        return SpeedCurve::constant(speed, spanUs);
    }
};

struct ExportReport {
    bool ok = false;
    QString error;
    QString outputPath;
    QString sidecarPath;
    int outputFrameCount = 0;
    int originalFrameCount = 0;
    int generatedFrameCount = 0;
    int freezeFrameCount = 0;
    int64_t outputDurationUs = 0;
    int64_t expectedDurationUs = 0;
    int64_t sourceSpanUs = 0;
    int startFrame = -1;
    int endFrame = -1;
    bool hasAudio = false;
    AudioMode audioMode = AudioMode::Mute;
    SpeedCurve curve;
    QVector<FrameProvenance> frames;
};

class Exporter {
public:
    explicit Exporter(VideoDecoder& decoder);

    bool run(const ExportSettings& s, ExportReport& report,
             std::function<void(int, int)> progress = nullptr);

    static QString audioModeName(AudioMode m);

private:
    VideoDecoder& m_dec;
};

} // namespace sr
