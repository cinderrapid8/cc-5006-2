#pragma once

#include "core/SpeedCurve.h"
#include "core/Types.h"
#include "core/VideoDecoder.h"
#include <QImage>
#include <QString>
#include <QVector>
#include <cstdint>
#include <functional>

namespace sr {

enum class SlowmoMode {
    Repeat = 0,      // explicit frame repetition, no synthesis
    Interpolate = 1, // linear blend between adjacent source frames
};

QString slowmoModeName(SlowmoMode m);

// Full per-frame origin record. Written to the .frames.json sidecar so the
// output mapping can be checked frame by frame against the source timeline.
struct FrameProvenance {
    int outputIndex = 0;
    int64_t outputPtsUs = 0;
    bool generated = true;       // true = produced by repeat/freeze/blend
    int sourceFrameA = -1;
    int sourceFrameB = -1;       // -1 in repeat/original/freeze cases
    double blendWeight = 0.0;    // weight of B in an interpolated frame
    int64_t sourceTimeUs = 0;    // absolute source timeline position shown
    int segmentIndex = -1;       // inverse-mapping segment this frame falls in
    double localSpeed = 1.0;     // 0.0 for a freeze segment
    QString kind;                // original | repeat | freeze | interpolated
};

// One output frame planned from the shared source->output mapping.
// The same record drives the on-screen preview cache and the exporter, so
// the frames shown while editing are exactly the frames rendered.
struct PlannedFrame {
    FrameProvenance prov;
    // Decode-time payload, filled by the exporter when it materialises the
    // interval. Preview planning leaves this empty and keeps only prov.
    QImage image;
};

struct FramePlan {
    bool ok = false;
    QString error;
    int64_t outputDurationUs = 0;   // mapping length
    int64_t sourceSpanUs = 0;       // source interval length
    int outputFrameRate = 60;
    int64_t outputStepUs = 0;
    QVector<PlannedFrame> frames;
};

// Stateless planner. All times are resolved against the decoder's real
// per-frame PTS index; average frame rate is never used for sampling.
class FramePlanner {
public:
    // Plan provenance only (no image decode): cheap enough for the preview
    // cache and the live output-length readout.
    static FramePlan plan(const QVector<FrameTime>& index,
                          int startFrame, int endFrame,
                          const SpeedCurve& curve,
                          int outputFrameRate,
                          SlowmoMode mode,
                          QString* error = nullptr);

    // Attach decoded images to a previously planned interval. `images` holds
    // the decoded source frames startFrame..endFrame in order.
    static void materialise(FramePlan& plan,
                            const QVector<FrameTime>& index,
                            int startFrame,
                            const QVector<QImage>& images,
                            SlowmoMode mode);

    // Convenience used by the preview widget for a single output position.
    // Returns the source selection that the mapping makes at output index.
    static FrameProvenance sampleAt(const FramePlan& plan, int outputIndex);
};

} // namespace sr
