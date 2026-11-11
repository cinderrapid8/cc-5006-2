#pragma once

#include "core/Types.h"
#include <QObject>
#include <QVector>

namespace sr {

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

    static constexpr int fileFormatVersion = 1;

private:
    QString m_path;
    SourceSummary m_source;
    QVector<Marker> m_markers;
    int m_nextMarkerId = 1;
};

} // namespace sr
