#pragma once

#include <QImage>
#include <QString>
#include <QVector>
#include <QJsonObject>
#include <cstdint>

namespace sr {

// Exact presentation time of a source frame. Never derived from index / avg fps.
struct FrameTime {
    int index = -1;            // unique zero-based decode order index
    int64_t ptsUs = -1;        // actual presentation timestamp in microseconds
    int64_t durationUs = 0;    // actual frame duration (next pts - this pts)
};

enum class MarkerPhase {
    Takeoff = 0,
    Air = 1,
    Landing = 2,
};
QString markerPhaseName(MarkerPhase p);

enum class MarkerState {
    Active = 0,   // bound to a verified source
    Pending = 1,  // source replaced / not yet verified
};

struct Marker {
    int id = -1;
    int frameIndex = -1;
    int64_t ptsUs = -1;            // actual pts, duplicated in index to survive reload
    MarkerPhase phase = MarkerPhase::Takeoff;
    QString note;
    MarkerState state = MarkerState::Active;
};

struct SourceSummary {
    QString path;
    qint64 sizeBytes = -1;
    qint64 mtimeSec = -1;
    QString sha256;                // full file digest used as identity
    int width = 0;
    int height = 0;
    int videoStreamIndex = -1;
    int audioStreamIndex = -1;     // -1: no audio track
    double avgFrameRate = 0.0;     // informational only, never used for timing
    int64_t durationUs = 0;
    bool truncatedTail = false;    // decode reported errors near end of file
};

QJsonObject markerToJson(const Marker& m);
Marker markerFromJson(const QJsonObject& o);

QString formatTimeUs(int64_t us);   // h:mm:ss.mmmmmm (µs precision shown as fractional)

} // namespace sr
