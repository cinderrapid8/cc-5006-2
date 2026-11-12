// srt_curve_demo: end-to-end acceptance driver for the segmented speed curve.
//
// It opens a real (VFR) source, builds a curve that enters at normal speed,
// decelerates sharply inside a source interval only one frame-gap wide,
// holds an explicit freeze, then accelerates out, and renders it through the
// exact same FramePlanner/Exporter pipeline the GUI uses. The resulting
// .frames.json sidecar is then inspectable frame-by-frame with:
//
//   srt_probe mapping <output>.mkv.frames.json
//   srt_probe mapping <output>.mkv.frames.json --at-us <outputUs>
//
// Usage:
//   srt_curve_demo --input clip.mkv --output out.mkv [--fps 120]
//                  [--slow-a 5] [--slow-b 6] [--freeze-us 150000]
//                  [--audio-mode mute|drop|atempo]
//                  [--freeze-audio silence|hold]
#include "core/Exporter.h"
#include "core/FramePlanner.h"
#include "core/SpeedCurve.h"
#include "core/VideoDecoder.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <cstdio>

using namespace sr;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QString input, output;
    int fps = 120;
    int slowA = 5, slowB = 6;
    int64_t freezeUs = 150'000;
    AudioMode audioMode = AudioMode::Mute;
    AudioFreezePolicy freezePolicy = AudioFreezePolicy::Silence;

    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i];
        auto next = [&](const char* name) -> QString {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", name); exit(2); }
            return QString::fromLocal8Bit(argv[++i]);
        };
        if (a == "--input") input = next("--input");
        else if (a == "--output") output = next("--output");
        else if (a == "--fps") fps = next("--fps").toInt();
        else if (a == "--slow-a") slowA = next("--slow-a").toInt();
        else if (a == "--slow-b") slowB = next("--slow-b").toInt();
        else if (a == "--freeze-us") freezeUs = next("--freeze-us").toLongLong();
        else if (a == "--audio-mode") {
            const QString v = next("--audio-mode");
            if (v == "drop") audioMode = AudioMode::Drop;
            else if (v == "atempo") audioMode = AudioMode::Atempo;
        }
        else if (a == "--freeze-audio") {
            const QString v = next("--freeze-audio");
            if (v == "hold") freezePolicy = AudioFreezePolicy::HoldLast;
        }
    }
    if (input.isEmpty() || output.isEmpty()) {
        fprintf(stderr, "usage: srt_curve_demo --input c.mkv --output o.mkv\n");
        return 2;
    }

    VideoDecoder dec;
    QString err;
    if (!dec.open(input, &err) || !dec.buildIndex(nullptr, &err)) {
        fprintf(stderr, "open failed: %s\n", qPrintable(err));
        return 1;
    }
    const auto& idx = dec.index();
    if (slowB >= dec.frameCount()) slowB = dec.frameCount() - 1;
    if (slowA >= slowB) slowA = slowB - 1;

    const int startFrame = 0;
    const int endFrame = dec.frameCount() - 1;
    const int64_t spanStart = idx[startFrame].ptsUs;
    const int64_t span = idx[endFrame].ptsUs + idx[endFrame].durationUs - spanStart;
    // The narrow deceleration band is exactly one real source frame gap wide;
    // sampling inside it must follow PTS, never an assumed constant fps.
    const int64_t bandA = idx[slowA].ptsUs - spanStart;
    const int64_t bandB = idx[slowB].ptsUs - spanStart;

    SpeedCurve curve;
    curve.setKnots({
        {0, 1.0, 0},
        {bandA, 0.02, 0},                                  // sharp deceleration
        {bandB, 0.6, freezeUs},                            // one-gap band, then hold
        {span, 1.0, 0},                                    // accelerate to exit
    });
    QString reason;
    if (!curve.validate(span, &reason)) {
        fprintf(stderr, "curve rejected: %s\n", qPrintable(reason));
        return 1;
    }

    ExportSettings s;
    s.outputPath = output;
    s.startFrame = startFrame;
    s.endFrame = endFrame;
    s.hasCurve = true;
    s.curve = curve;
    s.slowmoMode = SlowmoMode::Repeat;
    s.audioMode = audioMode;
    s.audioFreezePolicy = freezePolicy;
    s.outputFrameRate = fps;

    Exporter ex(dec);
    ExportReport rep;
    if (!ex.run(s, rep)) {
        fprintf(stderr, "export failed: %s\n", qPrintable(rep.error));
        return 1;
    }

    printf("source span      : %lld us\n", (long long)span);
    printf("narrow band      : [%lld, %lld) us  (gap %lld us, source frames %d..%d)\n",
           (long long)bandA, (long long)bandB, (long long)(bandB - bandA), slowA, slowB);
    printf("output duration  : %lld us across %d frames @ %d fps\n",
           (long long)rep.outputDurationUs, rep.outputFrameCount, fps);
    printf("freeze frames    : %d\n", rep.freezeFrameCount);
    printf("sidecar          : %s\n", qPrintable(rep.sidecarPath));
    printf("inspect with     : srt_probe mapping %s\n", qPrintable(rep.sidecarPath));
    return 0;
}
