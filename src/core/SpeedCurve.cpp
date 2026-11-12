#include "core/SpeedCurve.h"

#include <QJsonArray>
#include <cmath>
#include <limits>

namespace sr {

QString audioFreezePolicyName(AudioFreezePolicy p)
{
    switch (p) {
    case AudioFreezePolicy::Silence:  return QStringLiteral("silence");
    case AudioFreezePolicy::HoldLast: return QStringLiteral("hold-last-sample");
    case AudioFreezePolicy::Mute:     return QStringLiteral("mute");
    }
    return QStringLiteral("silence");
}

AudioFreezePolicy audioFreezePolicyFromName(const QString& name)
{
    if (name == QLatin1String("hold-last-sample")) return AudioFreezePolicy::HoldLast;
    if (name == QLatin1String("mute")) return AudioFreezePolicy::Mute;
    return AudioFreezePolicy::Silence;
}

SpeedCurve SpeedCurve::constant(double speed, int64_t spanUs)
{
    SpeedCurve c;
    c.m_knots = {{0, speed, 0}, {spanUs, speed, 0}};
    return c;
}

bool SpeedCurve::isConstant() const
{
    if (m_knots.size() < 2) return false;
    const double v = m_knots.first().speed;
    for (const auto& k : m_knots) {
        if (k.freezeDurationUs != 0) return false;
        if (std::fabs(k.speed - v) > 1e-12) return false;
    }
    return true;
}

double SpeedCurve::constantSpeed() const { return m_knots.first().speed; }

bool SpeedCurve::validate(int64_t spanUs, QString* reason) const
{
    auto fail = [&](const QString& msg) {
        if (reason) *reason = msg;
        return false;
    };
    if (spanUs <= 0)
        return fail(QStringLiteral("source span must be positive"));
    if (m_knots.size() < 2)
        return fail(QStringLiteral("speed curve needs at least two control points"));
    if (m_knots.first().timeUs != 0)
        return fail(QStringLiteral("speed curve must start at source time 0"));
    if (m_knots.last().timeUs != spanUs)
        return fail(QStringLiteral("speed curve must end at the interval end (%1 us)")
                        .arg(spanUs));

    double totalOut = 0.0;
    for (int i = 0; i < m_knots.size(); ++i) {
        const auto& k = m_knots[i];
        if (!std::isfinite(k.speed) || k.speed < minSpeed)
            return fail(QStringLiteral("control point %1 has rejected speed %2: "
                                       "negative and zero/infinite-stall speeds are unsupported")
                            .arg(i).arg(k.speed));
        if (k.speed > maxSpeed)
            return fail(QStringLiteral("control point %1 speed %2 exceeds limit %3")
                            .arg(i).arg(k.speed).arg(maxSpeed));
        if (!std::isfinite(double(k.freezeDurationUs)) || k.freezeDurationUs < 0)
            return fail(QStringLiteral("control point %1 has invalid freeze duration").arg(i));
        if (i > 0) {
            const int64_t gap = k.timeUs - m_knots[i - 1].timeUs;
            if (gap < minTimeGapUs)
                return fail(QStringLiteral("control points %1 and %2 coincide or are not "
                                           "ordered (source-time gap %3 us)")
                                .arg(i - 1).arg(i).arg(gap));
        }
        if (i + 1 < m_knots.size()) {
            const int64_t dt = m_knots[i + 1].timeUs - k.timeUs;
            const double segOut = dt / k.speed;
            if (!std::isfinite(segOut))
                return fail(QStringLiteral("segment %1 produces a non-finite output length; "
                                           "infinite-duration curves are unsupported").arg(i));
            totalOut += segOut + static_cast<double>(k.freezeDurationUs);
        }
    }
    if (!std::isfinite(totalOut) || totalOut > static_cast<double>(maxOutputDurationUs))
        return fail(QStringLiteral("output duration %1 s exceeds the supported limit of %2 s; "
                                   "refusing an effectively infinite curve")
                        .arg(totalOut / 1e6, 0, 'f', 3)
                        .arg(maxOutputDurationUs / 1'000'000));
    return true;
}

QVector<SpeedCurve::Segment> SpeedCurve::inverseSegments() const
{
    QVector<Segment> segs;
    int64_t outCursor = 0;
    for (int i = 0; i + 1 < m_knots.size(); ++i) {
        const auto& k = m_knots[i];
        const int64_t t0 = k.timeUs;
        const int64_t t1 = m_knots[i + 1].timeUs;
        if (k.freezeDurationUs > 0) {
            Segment fz;
            fz.outStartUs = outCursor;
            fz.outEndUs = outCursor + k.freezeDurationUs;
            fz.srcStartUs = t0;
            fz.srcEndUs = t0;
            fz.speed = 0.0;
            fz.knotIndex = i;
            segs.push_back(fz);
            outCursor = fz.outEndUs;
        }
        // Integer segment length keeps every boundary an exact microsecond.
        const int64_t lenOut = static_cast<int64_t>(
            std::llround(static_cast<double>(t1 - t0) / k.speed));
        Segment mv;
        mv.outStartUs = outCursor;
        mv.outEndUs = outCursor + qMax<int64_t>(1, lenOut);
        mv.srcStartUs = t0;
        mv.srcEndUs = t1;
        mv.speed = k.speed;
        mv.knotIndex = i;
        segs.push_back(mv);
        outCursor = mv.outEndUs;
    }
    const auto& last = m_knots.last();
    if (last.freezeDurationUs > 0) {
        Segment fz;
        fz.outStartUs = outCursor;
        fz.outEndUs = outCursor + last.freezeDurationUs;
        fz.srcStartUs = last.timeUs;
        fz.srcEndUs = last.timeUs;
        fz.speed = 0.0;
        fz.knotIndex = m_knots.size() - 1;
        segs.push_back(fz);
    }
    return segs;
}

int64_t SpeedCurve::outputDurationUs() const
{
    const auto segs = inverseSegments();
    return segs.isEmpty() ? 0 : segs.last().outEndUs;
}

int64_t SpeedCurve::outputToSource(int64_t sUs) const
{
    const auto segs = inverseSegments();
    if (segs.isEmpty()) return 0;
    if (sUs <= 0) return segs.first().srcStartUs;
    const int64_t total = segs.last().outEndUs;
    if (sUs >= total) return segs.last().srcEndUs;
    for (const auto& seg : segs) {
        if (sUs < seg.outEndUs) {
            if (seg.speed == 0.0)
                return seg.srcStartUs;
            const int64_t d = sUs - seg.outStartUs;
            const int64_t u = seg.srcStartUs + static_cast<int64_t>(
                std::llround(static_cast<double>(d) * seg.speed));
            return qBound(seg.srcStartUs, u, seg.srcEndUs);
        }
    }
    return segs.last().srcEndUs;
}

int64_t SpeedCurve::sourceToOutput(int64_t uUs) const
{
    if (m_knots.size() < 2) return 0;
    if (uUs <= 0) return m_knots.first().freezeDurationUs;
    int64_t outCursor = 0;
    for (int i = 0; i + 1 < m_knots.size(); ++i) {
        const auto& k = m_knots[i];
        const int64_t t0 = k.timeUs;
        const int64_t t1 = m_knots[i + 1].timeUs;
        if (uUs < t1 || i + 2 == m_knots.size()) {
            const int64_t clamped = qBound(t0, uUs, t1);
            const int64_t segLen = qMax<int64_t>(
                1, static_cast<int64_t>(
                       std::llround(static_cast<double>(t1 - t0) / k.speed)));
            const int64_t into = static_cast<int64_t>(
                std::llround(static_cast<double>(clamped - t0) / k.speed));
            // Right-continuous at the freeze anchor: the hold is included.
            return outCursor + k.freezeDurationUs + qMin(into, segLen);
        }
        outCursor += k.freezeDurationUs + qMax<int64_t>(
            1, static_cast<int64_t>(
                   std::llround(static_cast<double>(t1 - t0) / k.speed)));
    }
    return outCursor + m_knots.last().freezeDurationUs;
}

QJsonObject SpeedCurve::toJson() const
{
    QJsonObject o;
    QJsonArray arr;
    for (const auto& k : m_knots) {
        QJsonObject ko;
        ko["timeUs"] = static_cast<qint64>(k.timeUs);
        ko["speed"] = k.speed;
        ko["freezeDurationUs"] = static_cast<qint64>(k.freezeDurationUs);
        arr.append(ko);
    }
    o["knots"] = arr;
    return o;
}

SpeedCurve SpeedCurve::fromJson(const QJsonObject& o)
{
    SpeedCurve c;
    QVector<SpeedKnot> knots;
    const QJsonArray arr = o.value("knots").toArray();
    for (const auto& v : arr) {
        const QJsonObject ko = v.toObject();
        SpeedKnot k;
        k.timeUs = static_cast<int64_t>(ko.value("timeUs").toDouble());
        k.speed = ko.value("speed").toDouble(1.0);
        k.freezeDurationUs = static_cast<int64_t>(
            ko.value("freezeDurationUs").toDouble(0));
        knots.push_back(k);
    }
    c.m_knots = knots;
    return c;
}

} // namespace sr
