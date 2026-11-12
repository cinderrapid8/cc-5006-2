#include "core/FramePlanner.h"

#include <QImage>
#include <algorithm>
#include <cmath>

namespace sr {

QString slowmoModeName(SlowmoMode m)
{
    return m == SlowmoMode::Interpolate
               ? QStringLiteral("interpolate (linear blend)")
               : QStringLiteral("repeat (duplicate frames)");
}

static QImage blendImages(const QImage& a, const QImage& b, double w)
{
    QImage out(a.size(), QImage::Format_RGB888);
    const int bytes = a.width() * a.height() * 3;
    const auto* pa = a.constBits();
    const auto* pb = b.constBits();
    auto* po = out.bits();
    const int wa = static_cast<int>((1.0 - w) * 256.0 + 0.5);
    const int wb = 256 - wa;
    for (int i = 0; i < bytes; ++i)
        po[i] = static_cast<uint8_t>((pa[i] * wa + pb[i] * wb) >> 8);
    return out;
}

FramePlan FramePlanner::plan(const QVector<FrameTime>& index,
                             int startFrame, int endFrame,
                             const SpeedCurve& curve,
                             int outputFrameRate,
                             SlowmoMode mode,
                             QString* error)
{
    FramePlan plan;
    auto fail = [&](const QString& msg) {
        plan.ok = false;
        plan.error = msg;
        if (error) *error = msg;
        return plan;
    };
    if (startFrame < 0 || endFrame >= index.size() || startFrame > endFrame)
        return fail(QStringLiteral("invalid frame range [%1,%2], count=%3")
                        .arg(startFrame).arg(endFrame).arg(index.size()));

    const int64_t spanStartUs = index[startFrame].ptsUs;
    const int64_t sourceEndUs = index[endFrame].ptsUs + index[endFrame].durationUs;
    const int64_t spanUs = sourceEndUs - spanStartUs;

    QString curveError;
    if (!curve.validate(spanUs, &curveError))
        return fail(curveError);

    const int fps = qBound(1, outputFrameRate, 120);
    const int64_t stepUs = 1'000'000 / fps;
    const int64_t durationUs = curve.outputDurationUs();
    if (durationUs <= 0)
        return fail(QStringLiteral("curve produced an empty output timeline"));

    const auto segs = curve.inverseSegments();
    const int totalOut = qMax(1, static_cast<int>(durationUs / stepUs));
    plan.frames.reserve(totalOut);
    plan.outputFrameRate = fps;
    plan.outputStepUs = stepUs;
    plan.outputDurationUs = durationUs;
    plan.sourceSpanUs = spanUs;

    // Span-relative PTS of source frame j (0 <= j <= count-1).
    auto srcTimeRel = [&](int j) -> int64_t {
        return index[startFrame + j].ptsUs - spanStartUs;
    };
    const int srcCount = endFrame - startFrame + 1;

    // Locate the source interval containing span-relative time u.
    // Uses the real PTS index, so VFR gaps and coincident curve positions are
    // resolved by timestamps rather than an assumed constant frame rate.
    auto locate = [&](int64_t u) -> int {
        int lo = 0, hi = srcCount - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (srcTimeRel(mid) <= u) lo = mid;
            else hi = mid - 1;
        }
        return lo;
    };

    int64_t lastSourceRel = -1;
    int outCount = 0;
    while (true) {
        const int64_t outPts = outCount * stepUs;
        if (outPts >= durationUs) break;

        // Segment lookup by output time; segments tile [0, durationUs).
        int segIdx = 0;
        while (segIdx + 1 < segs.size() && segs[segIdx].outEndUs <= outPts)
            ++segIdx;
        const auto& seg = segs[segIdx];

        const int64_t srcRel = qBound<int64_t>(0, curve.outputToSource(outPts),
                                                spanUs);
        // Defensive monotonicity: the mapping is monotone, integer rounding
        // must never make a later output frame point backwards in the source.
        const int64_t clampedRel = qMax(srcRel, lastSourceRel);
        lastSourceRel = clampedRel;

        int j = locate(clampedRel);
        const int64_t t0 = srcTimeRel(j);
        const int64_t t1 = (j + 1 < srcCount) ? srcTimeRel(j + 1) : spanUs;
        const double frac = (t1 > t0)
                                ? static_cast<double>(clampedRel - t0)
                                      / static_cast<double>(t1 - t0)
                                : 0.0;
        const bool insideFreeze = seg.speed == 0.0;
        const bool exactSource = std::fabs(double(clampedRel - t0)) < 1.0
                                 && frac <= 0.001;

        FrameProvenance p;
        p.outputIndex = outCount;
        p.outputPtsUs = outPts;
        p.sourceFrameA = startFrame + j;
        p.sourceTimeUs = spanStartUs + clampedRel;
        p.segmentIndex = segIdx;
        p.localSpeed = seg.speed;

        if (insideFreeze) {
            // Explicit hold: the mapping has zero source velocity here.
            p.generated = true;
            p.sourceFrameB = -1;
            p.kind = QStringLiteral("freeze");
        } else if (mode == SlowmoMode::Interpolate && j + 1 < srcCount
                   && frac > 0.001) {
            p.generated = true;
            p.sourceFrameB = startFrame + j + 1;
            p.blendWeight = frac;
            p.kind = QStringLiteral("interpolated");
        } else if (exactSource) {
            p.generated = false;
            p.sourceFrameB = -1;
            p.kind = QStringLiteral("original");
        } else {
            p.generated = true;
            p.sourceFrameB = -1;
            p.kind = QStringLiteral("repeat");
        }
        plan.frames.push_back({p, QImage{}});
        ++outCount;
    }

    if (plan.frames.isEmpty())
        return fail(QStringLiteral("no output frames planned"));
    plan.ok = true;
    return plan;
}

void FramePlanner::materialise(FramePlan& plan,
                               const QVector<FrameTime>& index,
                               int startFrame,
                               const QVector<QImage>& images,
                               SlowmoMode mode)
{
    if (!plan.ok) return;
    for (auto& pf : plan.frames) {
        const int j = pf.prov.sourceFrameA - startFrame;
        if (j < 0 || j >= images.size()) continue;
        const auto& p = pf.prov;
        if (p.kind == QLatin1String("interpolated")) {
            const int k = p.sourceFrameB - startFrame;
            if (k >= 0 && k < images.size())
                pf.image = blendImages(images[j], images[k], p.blendWeight);
            else
                pf.image = images[j];
        } else {
            // original / repeat / freeze all display one decoded source frame.
            pf.image = images[j];
        }
    }
}

FrameProvenance FramePlanner::sampleAt(const FramePlan& plan, int outputIndex)
{
    if (outputIndex < 0 || outputIndex >= plan.frames.size())
        return {};
    return plan.frames[outputIndex].prov;
}

} // namespace sr
