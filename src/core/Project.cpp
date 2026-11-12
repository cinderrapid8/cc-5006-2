#include "core/Project.h"
#include "core/VideoDecoder.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace sr {

static QJsonObject sourceToJson(const SourceSummary& s)
{
    QJsonObject o;
    o["path"] = s.path;
    o["sizeBytes"] = static_cast<qint64>(s.sizeBytes);
    o["mtimeSec"] = static_cast<qint64>(s.mtimeSec);
    o["sha256"] = s.sha256;
    o["width"] = s.width;
    o["height"] = s.height;
    o["videoStreamIndex"] = s.videoStreamIndex;
    o["audioStreamIndex"] = s.audioStreamIndex;
    o["avgFrameRate"] = s.avgFrameRate;
    o["durationUs"] = static_cast<qint64>(s.durationUs);
    o["truncatedTail"] = s.truncatedTail;
    return o;
}

static SourceSummary sourceFromJson(const QJsonObject& o)
{
    SourceSummary s;
    s.path = o.value("path").toString();
    s.sizeBytes = static_cast<qint64>(o.value("sizeBytes").toDouble(-1));
    s.mtimeSec = static_cast<qint64>(o.value("mtimeSec").toDouble(-1));
    s.sha256 = o.value("sha256").toString();
    s.width = o.value("width").toInt();
    s.height = o.value("height").toInt();
    s.videoStreamIndex = o.value("videoStreamIndex").toInt(-1);
    s.audioStreamIndex = o.value("audioStreamIndex").toInt(-1);
    s.avgFrameRate = o.value("avgFrameRate").toDouble();
    s.durationUs = static_cast<int64_t>(o.value("durationUs").toDouble());
    s.truncatedTail = o.value("truncatedTail").toBool();
    return s;
}

Project::Project() = default;

QJsonObject ExportConfig::toJson() const
{
    QJsonObject o;
    o["startFrame"] = startFrame;
    o["endFrame"] = endFrame;
    o["hasCurve"] = hasCurve;
    o["speed"] = speed;
    o["slowmoMode"] = slowmoMode;
    o["audioMode"] = audioMode;
    o["audioFreezePolicy"] = audioFreezePolicy;
    o["outputFrameRate"] = outputFrameRate;
    o["speedCurve"] = curve.toJson();
    return o;
}

ExportConfig ExportConfig::fromJson(const QJsonObject& o)
{
    ExportConfig c;
    c.startFrame = o.value("startFrame").toInt(0);
    c.endFrame = o.value("endFrame").toInt(0);
    c.hasCurve = o.value("hasCurve").toBool(false);
    c.speed = o.value("speed").toDouble(0.5);
    c.slowmoMode = o.value("slowmoMode").toInt(0);
    c.audioMode = o.value("audioMode").toInt(0);
    c.audioFreezePolicy = o.value("audioFreezePolicy").toInt(0);
    c.outputFrameRate = o.value("outputFrameRate").toInt(60);
    if (o.contains("speedCurve"))
        c.curve = SpeedCurve::fromJson(o.value("speedCurve").toObject());
    return c;
}

int Project::addMarker(int frameIndex, int64_t ptsUs, MarkerPhase phase, const QString& note)
{
    if (auto* existing = markerAt(frameIndex)) {
        existing->phase = phase;
        existing->ptsUs = ptsUs;
        existing->state = MarkerState::Active;
        if (!note.isEmpty()) existing->note = note;
        return existing->id;
    }
    Marker m;
    m.id = m_nextMarkerId++;
    m.frameIndex = frameIndex;
    m.ptsUs = ptsUs;
    m.phase = phase;
    m.note = note;
    m.state = MarkerState::Active;
    m_markers.push_back(m);
    return m.id;
}

void Project::removeMarker(int id)
{
    for (int i = 0; i < m_markers.size(); ++i) {
        if (m_markers[i].id == id) { m_markers.removeAt(i); return; }
    }
}

bool Project::hasMarkerAt(int frameIndex) const
{
    for (const auto& m : m_markers)
        if (m.frameIndex == frameIndex) return true;
    return false;
}

Marker* Project::markerAt(int frameIndex)
{
    for (auto& m : m_markers)
        if (m.frameIndex == frameIndex) return &m;
    return nullptr;
}

bool Project::verifySource(QString* detail)
{
    QFileInfo fi(m_source.path);
    if (!fi.exists()) {
        if (detail) *detail = QStringLiteral("source file missing: %1").arg(m_source.path);
        for (auto& m : m_markers) m.state = MarkerState::Pending;
        return false;
    }
    QString err;
    const QString digest = sha256OfFile(m_source.path, &err);
    if (digest.isEmpty() || digest.compare(m_source.sha256, Qt::CaseInsensitive) != 0) {
        if (detail) {
            *detail = digest.isEmpty()
                ? QStringLiteral("cannot hash source: %1").arg(err)
                : QStringLiteral("source digest changed (file replaced): %1").arg(m_source.path);
        }
        // Material replacement: old markers must be reviewed, never silently
        // treated as valid against the new content.
        for (auto& m : m_markers) m.state = MarkerState::Pending;
        return false;
    }
    for (auto& m : m_markers) m.state = MarkerState::Active;
    if (detail) detail->clear();
    return true;
}

void Project::replaceSource(const SourceSummary& newSource)
{
    m_source = newSource;
    for (auto& m : m_markers) m.state = MarkerState::Pending;
}

bool Project::save(const QString& filePath, QString* err)
{
    QJsonObject root;
    root["formatVersion"] = fileFormatVersion;
    root["source"] = sourceToJson(m_source);
    QJsonArray arr;
    for (const auto& m : m_markers) arr.append(markerToJson(m));
    root["markers"] = arr;
    root["nextMarkerId"] = m_nextMarkerId;
    if (m_hasExport)
        root["exportConfig"] = m_export.toJson();

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QStringLiteral("cannot write %1: %2").arg(filePath, f.errorString());
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    m_path = filePath;
    return true;
}

bool Project::load(const QString& filePath, QString* err)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("cannot read %1: %2").arg(filePath, f.errorString());
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        if (err) *err = QStringLiteral("not a valid project file");
        return false;
    }
    const QJsonObject root = doc.object();
    const int version = root.value("formatVersion").toInt();
    // v1 projects predate segmented speed curves. They carry no curve, so the
    // legacy constant-speed path is preserved and re-renders identically.
    if (version != fileFormatVersion && version != legacyFileFormatVersion) {
        if (err) *err = QStringLiteral("unsupported project format version");
        return false;
    }
    m_source = sourceFromJson(root.value("source").toObject());
    m_markers.clear();
    const QJsonArray arr = root.value("markers").toArray();
    for (const auto& v : arr)
        m_markers.push_back(markerFromJson(v.toObject()));
    m_nextMarkerId = root.value("nextMarkerId").toInt(m_markers.size() + 1);
    if (root.contains("exportConfig")) {
        m_export = ExportConfig::fromJson(root.value("exportConfig").toObject());
        m_hasExport = true;
    } else {
        m_export = ExportConfig{};
        m_hasExport = false;
    }
    m_path = filePath;
    return true;
}

} // namespace sr
