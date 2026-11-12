// srt_probe: independent command-line inspection of a video file or an
// export produced by sportreview.
//
// Modes:
//   srt_probe info   <file>                 stream/container summary (JSON)
//   srt_probe frames <file> [--limit N]     every frame's real pts/duration
//   srt_probe verify <export> [sidecar]     re-decode the export and check
//                                           the .frames.json provenance
//   srt_probe readid <file> <frameIndex>    decode one exact frame and read
//                                           its embedded synthetic frame id
//   srt_probe mapping <sidecar> [--at-us S] dump the speed-curve mapping and
//                                           the source time shown at output
//                                           position S microseconds (or every
//                                           planned frame when --at-us is out)
//
// Output is JSON on stdout so the test suite can parse it without Qt widgets.
#include "core/VideoDecoder.h"
#include "core/SpeedCurve.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cstdio>

using namespace sr;

static QJsonObject summaryJson(const SourceSummary& s)
{
    QJsonObject o;
    o["path"] = s.path;
    o["width"] = s.width;
    o["height"] = s.height;
    o["videoStreamIndex"] = s.videoStreamIndex;
    o["audioStreamIndex"] = s.audioStreamIndex;
    o["hasAudio"] = s.audioStreamIndex >= 0;
    o["avgFrameRate"] = s.avgFrameRate;
    o["durationUs"] = static_cast<qint64>(s.durationUs);
    o["truncatedTail"] = s.truncatedTail;
    o["sha256"] = s.sha256;
    return o;
}

// Read the 10-bit barcode painted by srt_genclip out of a decoded frame.
static int readEmbeddedFrameId(const QImage& img)
{
    const int barTop = 16, barBottom = 96, barW = 40, startX = 20;
    auto whiteAt = [&](int cx) -> bool {
        long long sum = 0, n = 0;
        for (int y = barTop + 8; y < barBottom - 8; y += 6) {
            const auto* px = img.constScanLine(y);
            for (int x = cx - 8; x <= cx + 8; x += 4) {
                sum += px[x * 3] + px[x * 3 + 1] + px[x * 3 + 2];
                ++n;
            }
        }
        return (sum / n) > 3 * 96; // average channel value > 96
    };
    int id = 0;
    int x = startX + barW + 8 + barW / 2;
    for (int bit = 0; bit < 10; ++bit) {
        if (whiteAt(x)) id |= (1 << bit);
        x += barW + 8;
    }
    return id;
}

static int cmdInfo(const QString& path)
{
    VideoDecoder dec;
    QString err;
    if (!dec.open(path, &err)) {
        QJsonObject e; e["error"] = err;
        printf("%s\n", QJsonDocument(e).toJson(QJsonDocument::Compact).constData());
        return 1;
    }
    if (!dec.buildIndex(nullptr, &err)) {
        QJsonObject e; e["error"] = err;
        printf("%s\n", QJsonDocument(e).toJson(QJsonDocument::Compact).constData());
        return 1;
    }
    QJsonObject out = summaryJson(dec.summary());
    out["frameCount"] = dec.frameCount();
    const auto& idx = dec.index();
    QSet<int64_t> durations;
    for (const auto& ft : idx) durations.insert(ft.durationUs);
    out["distinctDurationCount"] = durations.size();
    out["isVariableFrameRate"] = durations.size() > 1;
    int64_t minD = INT64_MAX, maxD = 0;
    for (const auto& ft : idx) { minD = qMin(minD, ft.durationUs); maxD = qMax(maxD, ft.durationUs); }
    out["minFrameDurationUs"] = static_cast<qint64>(minD);
    out["maxFrameDurationUs"] = static_cast<qint64>(maxD);
    out["lastFramePtsUs"] = static_cast<qint64>(idx.last().ptsUs);
    printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Indented).constData());
    return 0;
}

static int cmdFrames(const QString& path, int limit)
{
    VideoDecoder dec;
    QString err;
    if (!dec.open(path, &err) || !dec.buildIndex(nullptr, &err)) {
        QJsonObject e; e["error"] = err;
        printf("%s\n", QJsonDocument(e).toJson(QJsonDocument::Compact).constData());
        return 1;
    }
    QJsonObject out = summaryJson(dec.summary());
    QJsonArray arr;
    int n = limit > 0 ? qMin(limit, dec.frameCount()) : dec.frameCount();
    for (int i = 0; i < n; ++i) {
        const FrameTime& ft = dec.index()[i];
        QJsonObject fo;
        fo["index"] = ft.index;
        fo["ptsUs"] = static_cast<qint64>(ft.ptsUs);
        fo["durationUs"] = static_cast<qint64>(ft.durationUs);
        arr.append(fo);
    }
    out["frames"] = arr;
    out["frameCount"] = dec.frameCount();
    printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Indented).constData());
    return 0;
}

static int cmdVerify(const QString& exportPath, const QString& sidecarArg)
{
    QString sidecar = sidecarArg.isEmpty() ? exportPath + ".frames.json" : sidecarArg;
    QFile sf(sidecar);
    if (!sf.open(QIODevice::ReadOnly)) {
        QJsonObject e; e["error"] = QStringLiteral("cannot open sidecar %1").arg(sidecar);
        printf("%s\n", QJsonDocument(e).toJson(QJsonDocument::Compact).constData());
        return 1;
    }
    const QJsonObject meta = QJsonDocument::fromJson(sf.readAll()).object();
    const QJsonArray expected = meta.value("frames").toArray();

    VideoDecoder dec;
    QString err;
    if (!dec.open(exportPath, &err) || !dec.buildIndex(nullptr, &err)) {
        QJsonObject e; e["error"] = err;
        printf("%s\n", QJsonDocument(e).toJson(QJsonDocument::Compact).constData());
        return 1;
    }

    QJsonObject out;
    out["export"] = exportPath;
    out["sidecar"] = sidecar;
    out["decodedFrameCount"] = dec.frameCount();
    out["sidecarFrameCount"] = expected.size();
    out["truncatedTail"] = dec.summary().truncatedTail;
    out["hasAudio"] = dec.summary().audioStreamIndex >= 0;

    int mismatches = 0;
    int64_t lastEndUs = 0;
    QJsonArray problems;
    // Containers such as Matroska quantise PTS to a millisecond grid, and
    // audio interleaving can shift the whole video track by a constant start
    // offset. Verify the relative timeline (each frame minus the first frame)
    // so ordering and exact inter-frame spacing are checked, while tolerating
    // only the documented per-tick quantisation.
    const int64_t tickUs = static_cast<int64_t>(
        meta.value("containerTickUs").toDouble(1));
    const int64_t firstDecodedUs = dec.index().isEmpty()
        ? 0 : dec.index().first().ptsUs;
    const int64_t firstSidecarUs = expected.isEmpty()
        ? 0 : static_cast<int64_t>(expected.first().toObject()
                                       .value("outputPtsUs").toDouble());
    for (int i = 0; i < qMin(dec.frameCount(), expected.size()); ++i) {
        const QJsonObject ep = expected[i].toObject();
        const FrameTime& ft = dec.index()[i];
        const int64_t ePts = static_cast<int64_t>(ep.value("outputPtsUs").toDouble());
        const int64_t relDec = ft.ptsUs - firstDecodedUs;
        const int64_t relSide = ePts - firstSidecarUs;
        if (qAbs(relDec - relSide) > tickUs) {
            ++mismatches;
            QJsonObject p;
            p["frame"] = i;
            p["decodedPtsUs"] = static_cast<qint64>(ft.ptsUs);
            p["sidecarPtsUs"] = static_cast<qint64>(ePts);
            p["relativeDeltaUs"] = static_cast<qint64>(relDec - relSide);
            p["toleranceUs"] = static_cast<qint64>(tickUs);
            problems.append(p);
        }
        lastEndUs = ft.ptsUs + ft.durationUs;
    }
    if (dec.frameCount() != expected.size()) {
        ++mismatches;
        QJsonObject p;
        p["frameCountMismatch"] = true;
        p["decoded"] = dec.frameCount();
        p["sidecar"] = expected.size();
        problems.append(p);
    }
    out["ptsMismatches"] = mismatches;
    out["problems"] = problems;
    out["measuredDurationUs"] = static_cast<qint64>(lastEndUs);
    out["declaredDurationUs"] = meta.value("outputDurationUs");
    out["ok"] = mismatches == 0;
    printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Indented).constData());
    return mismatches == 0 ? 0 : 3;
}

static int cmdReadId(const QString& path, int frameIndex)
{
    VideoDecoder dec;
    QString err;
    if (!dec.open(path, &err) || !dec.buildIndex(nullptr, &err)) {
        QJsonObject e; e["error"] = err;
        printf("%s\n", QJsonDocument(e).toJson(QJsonDocument::Compact).constData());
        return 1;
    }
    DecodedFrame fr;
    if (!dec.frameAt(frameIndex, fr, &err)) {
        QJsonObject e; e["error"] = err;
        printf("%s\n", QJsonDocument(e).toJson(QJsonDocument::Compact).constData());
        return 1;
    }
    QJsonObject out;
    out["requestedIndex"] = frameIndex;
    out["decodedIndex"] = fr.index;
    out["ptsUs"] = static_cast<qint64>(fr.ptsUs);
    out["embeddedFrameId"] = readEmbeddedFrameId(fr.image);
    printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Indented).constData());
    return 0;
}

// Dump the source<->output time mapping stored in a sidecar. This is the
// frame-by-frame inspection entry point for segmented speed-curve exports:
// every planned output frame is listed with the exact source time and source
// frame it resolves to, and an arbitrary output position can be queried.
static int cmdMapping(const QString& sidecarPath, const QString& atUsArg)
{
    QFile sf(sidecarPath);
    if (!sf.open(QIODevice::ReadOnly)) {
        QJsonObject e; e["error"] = QStringLiteral("cannot open sidecar %1").arg(sidecarPath);
        printf("%s\n", QJsonDocument(e).toJson(QJsonDocument::Compact).constData());
        return 1;
    }
    const QJsonObject meta = QJsonDocument::fromJson(sf.readAll()).object();
    const int64_t spanUs = static_cast<int64_t>(meta.value("sourceSpanUs").toDouble());
    const SpeedCurve curve = SpeedCurve::fromJson(meta.value("speedCurve").toObject());
    QString reason;
    const bool valid = curve.validate(spanUs, &reason);

    QJsonObject out;
    out["sidecar"] = sidecarPath;
    out["valid"] = valid;
    if (!valid) out["reason"] = reason;
    out["sourceSpanUs"] = static_cast<qint64>(spanUs);
    out["outputDurationUs"] = static_cast<qint64>(curve.outputDurationUs());
    out["hasCurve"] = meta.value("hasCurve").toBool();
    QJsonArray segs;
    for (const auto& seg : curve.inverseSegments()) {
        QJsonObject so;
        so["outStartUs"] = static_cast<qint64>(seg.outStartUs);
        so["outEndUs"] = static_cast<qint64>(seg.outEndUs);
        so["srcStartUs"] = static_cast<qint64>(seg.srcStartUs);
        so["srcEndUs"] = static_cast<qint64>(seg.srcEndUs);
        so["speed"] = seg.speed;
        segs.append(so);
    }
    out["segments"] = segs;

    if (!atUsArg.isEmpty()) {
        bool parsed = false;
        const int64_t atUs = atUsArg.toLongLong(&parsed);
        if (!parsed) {
            QJsonObject e; e["error"] = QStringLiteral("invalid --at-us value");
            printf("%s\n", QJsonDocument(e).toJson(QJsonDocument::Compact).constData());
            return 2;
        }
        out["queryOutputUs"] = static_cast<qint64>(atUs);
        out["querySourceUs"] = static_cast<qint64>(curve.outputToSource(atUs));
    } else {
        QJsonArray farr;
        int64_t lastSrc = -1;
        bool monotone = true;
        for (const auto& v : meta.value("frames").toArray()) {
            const QJsonObject fo = v.toObject();
            const int64_t srcUs = static_cast<int64_t>(
                fo.value("sourceTimeUs").toDouble());
            if (srcUs < lastSrc) monotone = false;
            lastSrc = srcUs;
            QJsonObject row;
            row["outputIndex"] = fo.value("outputIndex").toInt();
            row["outputPtsUs"] = fo.value("outputPtsUs");
            row["sourceTimeUs"] = fo.value("sourceTimeUs");
            row["sourceFrameA"] = fo.value("sourceFrameA");
            row["sourceFrameB"] = fo.value("sourceFrameB");
            row["kind"] = fo.value("kind");
            row["segmentIndex"] = fo.value("segmentIndex");
            row["localSpeed"] = fo.value("localSpeed");
            farr.append(row);
        }
        out["frames"] = farr;
        out["sourceTimeMonotone"] = monotone;
    }
    printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Indented).constData());
    return valid ? 0 : 3;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage:\n"
                "  srt_probe info <file>\n"
                "  srt_probe frames <file> [--limit N]\n"
                "  srt_probe verify <export> [sidecar]\n"
                "  srt_probe readid <file> <frameIndex>\n"
                "  srt_probe mapping <sidecar> [--at-us US]\n");
        return 2;
    }
    const QString mode = argv[1];
    const QString path = argv[2];
    if (mode == "info") return cmdInfo(path);
    if (mode == "frames") {
        int limit = 0;
        for (int i = 3; i < argc; ++i)
            if (QString(argv[i]) == "--limit" && i + 1 < argc) limit = atoi(argv[++i]);
        return cmdFrames(path, limit);
    }
    if (mode == "verify")
        return cmdVerify(path, argc > 3 ? QString(argv[3]) : QString());
    if (mode == "readid") {
        if (argc < 4) return 2;
        return cmdReadId(path, atoi(argv[3]));
    }
    if (mode == "mapping") {
        QString atUs;
        for (int i = 3; i < argc; ++i)
            if (QString(argv[i]) == "--at-us" && i + 1 < argc) atUs = argv[++i];
        return cmdMapping(path, atUs);
    }
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}
