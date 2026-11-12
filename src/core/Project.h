#pragma once

#include "core/SpeedCurve.h"
#include "core/Types.h"
#include <QObject>
#include <QVector>

namespace sr {

// Persisted export configuration. The speed curve is saved as data, so a
// reopened project renders the same mapping (and therefore the same output
// duration and frame selection) without any re-approximation.
struct ExportConfig {
    int startFrame = 0;
    int endFrame = 0;
    bool hasCurve = false;
    SpeedCurve curve;
    double speed = 0.5;           // legacy constant speed, kept for v1 projects
    int slowmoMode = 0;           // sr::SlowmoMode as int
    int audioMode = 0;            // sr::AudioMode as int
    int audioFreezePolicy = 0;    // sr::AudioFreezePolicy as int
    int outputFrameRate = 60;

    QJsonObject toJson() const;
    static ExportConfig fromJson(const QJsonObject& o);
};

// A review project: markers bound to a source file via its digest and the
// exact presentation timestamps recorded at marking time.
class Project {
public:
    Project();

    const QString& path() const { return m_path; }
    void setPath(const QString& p) { m_path = p; }

    SourceSummary& source() { return m_source; }
    const SourceSummary& source() const { return m_source; }
    QVector<Marker>& markers() { return m_markers; }
    const QVector<Marker>& markers() const { return m_markers; }
    ExportConfig& exportConfig() { return m_export; }
    const ExportConfig& exportConfig() const { return m_export; }
    void setExportConfig(const ExportConfig& c) { m_export = c; m_hasExport = true; }
    bool hasExportConfig() const { return m_hasExport; }

    int addMarker(int frameIndex, int64_t ptsUs, MarkerPhase phase, const QString& note = {});
    void removeMarker(int id);
    bool hasMarkerAt(int frameIndex) const;
    Marker* markerAt(int frameIndex);

    // Check the on-disk source against the digest stored in the project.
    // Returns true when the file is identical. On mismatch every marker is
    // moved to Pending ("待核") and false is returned. Missing file -> false.
    bool verifySource(QString* detail = nullptr);

    // Re-bind to a replacement file (already opened/decoded by the caller).
    // Markers stay Pending until the user confirms them one by one.
    void replaceSource(const SourceSummary& newSource);

    bool save(const QString& filePath, QString* err = nullptr);
    bool load(const QString& filePath, QString* err = nullptr);

    // v1 projects predate the segmented speed curve; they are still accepted.
    // New writes use version 2.
    static constexpr int fileFormatVersion = 2;
    static constexpr int legacyFileFormatVersion = 1;

private:
    QString m_path;
    SourceSummary m_source;
    QVector<Marker> m_markers;
    ExportConfig m_export;
    bool m_hasExport = false;
    int m_nextMarkerId = 1;
};

} // namespace sr
