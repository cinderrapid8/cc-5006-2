#include "core/Types.h"

namespace sr {

QString markerPhaseName(MarkerPhase p)
{
    switch (p) {
    case MarkerPhase::Takeoff: return QStringLiteral("takeoff");
    case MarkerPhase::Air:     return QStringLiteral("air");
    case MarkerPhase::Landing: return QStringLiteral("landing");
    }
    return QStringLiteral("unknown");
}

QJsonObject markerToJson(const Marker& m)
{
    QJsonObject o;
    o["id"] = m.id;
    o["frameIndex"] = m.frameIndex;
    o["ptsUs"] = static_cast<qint64>(m.ptsUs);
    o["phase"] = markerPhaseName(m.phase);
    o["note"] = m.note;
    o["state"] = m.state == MarkerState::Pending ? "pending" : "active";
    return o;
}

Marker markerFromJson(const QJsonObject& o)
{
    Marker m;
    m.id = o.value("id").toInt(-1);
    m.frameIndex = o.value("frameIndex").toInt(-1);
    m.ptsUs = static_cast<int64_t>(o.value("ptsUs").toDouble(-1));
    const QString phase = o.value("phase").toString();
    if (phase == "air") m.phase = MarkerPhase::Air;
    else if (phase == "landing") m.phase = MarkerPhase::Landing;
    else m.phase = MarkerPhase::Takeoff;
    m.note = o.value("note").toString();
    m.state = o.value("state").toString("active") == "pending"
                  ? MarkerState::Pending : MarkerState::Active;
    return m;
}

QString formatTimeUs(int64_t us)
{
    if (us < 0) return QStringLiteral("--:--:--.------");
    bool neg = us < 0;
    qint64 u = neg ? -us : us;
    qint64 wholeSec = u / 1'000'000;
    qint64 frac = u % 1'000'000;
    qint64 h = wholeSec / 3600;
    qint64 min = (wholeSec % 3600) / 60;
    qint64 sec = wholeSec % 60;
    return (neg ? QStringLiteral("-") : QString())
        + QString::asprintf("%02lld:%02lld:%02lld.%06lld",
                            static_cast<long long>(h),
                            static_cast<long long>(min),
                            static_cast<long long>(sec),
                            static_cast<long long>(frac));
}

} // namespace sr
