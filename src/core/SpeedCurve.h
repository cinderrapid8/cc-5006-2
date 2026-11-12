#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <cstdint>

namespace sr {

// One breakpoint of the segmented speed curve.
//
// The curve is a mapping from source time u to output time s, expressed on
// the source timeline (all times in microseconds, relative to the start of
// the exported interval):
//
//   * On the half-open source interval [timeUs, next.timeUs) the local
//     playback speed is `speed` (source seconds per output second). A speed
//     of 1.0 plays at real rate, 0.25 stretches time 4x. The mapping slope
//     ds/du is 1/speed.
//   * `freezeDurationUs > 0` defines an explicit freeze (hold) at exactly
//     timeUs: source time stops while the output advances. It is the only
//     permitted non-strict part of the mapping. On the inverse timeline it
//     is a zero-speed (du/ds = 0) segment of the given output length.
//
// Knots are stored in strictly increasing timeUs order.
struct SpeedKnot {
    int64_t timeUs = 0;          // source-timeline position, span-relative
    double speed = 1.0;          // local speed on the segment after this knot
    int64_t freezeDurationUs = 0; // explicit hold length at timeUs (output us)
};

// What happens to the audio track while a video freeze is shown.
enum class AudioFreezePolicy {
    Silence = 0,  // insert silence for the whole freeze
    HoldLast = 1, // repeat the last source sample(s) through the freeze
    Mute = 2,     // keep the track but write silence (same samples as Silence,
                  // kept distinct so saved projects remember the intent)
};

QString audioFreezePolicyName(AudioFreezePolicy p);
AudioFreezePolicy audioFreezePolicyFromName(const QString& name);

class SpeedCurve {
public:
    // Validation bounds. Negative speeds are always rejected; a vanishing
    // speed would make the output duration infinite, so a hard floor keeps
    // every accepted curve finite.
    static constexpr double minSpeed = 1e-4;
    static constexpr double maxSpeed = 100.0;
    static constexpr int64_t maxOutputDurationUs = 3'600'000'000LL; // 1 hour
    static constexpr int64_t minTimeGapUs = 1;

    SpeedCurve() = default;

    // A single-knot curve covering [0, spanUs] at constant speed. This is
    // the form every pre-curve (legacy constant slow-motion) setting maps
    // to, and it reproduces the historical frame planning exactly.
    static SpeedCurve constant(double speed, int64_t spanUs);

    const QVector<SpeedKnot>& knots() const { return m_knots; }
    QVector<SpeedKnot>& knots() { return m_knots; }
    void setKnots(QVector<SpeedKnot> k) { m_knots = std::move(k); }

    bool isConstant() const;
    double constantSpeed() const; // only valid when isConstant()

    // Structural validation against a concrete source span.
    // spanUs is the source interval length in microseconds.
    // Returns false with a human-readable reason otherwise. Rejects:
    //   - empty curves and curves not anchored at 0 / spanUs,
    //   - non-increasing or coincident knot times,
    //   - non-finite, negative, zero (below minSpeed) or over-max speeds,
    //   - negative/non-finite freeze durations,
    //   - total output durations that are not finite / exceed the cap.
    bool validate(int64_t spanUs, QString* reason = nullptr) const;

    // Output time s reached at source time u (u in [0, spanUs]).
    // Freezes appear as a jump of freezeDurationUs at their anchor time.
    int64_t sourceToOutput(int64_t uUs) const;

    // Source time u shown at output time s (s in [0, outputDurationUs()]).
    // Inside a freeze the held source position is returned.
    int64_t outputToSource(int64_t sUs) const;

    // Total length of the output timeline, including freeze holds.
    int64_t outputDurationUs() const;

    // Inverse-mapping segments consumed by the frame planner and the audio
    // pipeline. Every entry covers a non-empty output interval; freeze
    // segments carry speed == 0.0 and constant source start/end.
    struct Segment {
        int64_t outStartUs = 0;
        int64_t outEndUs = 0;
        int64_t srcStartUs = 0;
        int64_t srcEndUs = 0;
        double speed = 1.0;       // 0.0 marks a freeze segment
        int knotIndex = -1;       // knot the segment originates from
    };
    QVector<Segment> inverseSegments() const;

    // Persistence. The JSON form is self-describing and version-tolerant.
    QJsonObject toJson() const;
    static SpeedCurve fromJson(const QJsonObject& o);

private:
    QVector<SpeedKnot> m_knots;
};

} // namespace sr
