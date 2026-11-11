// Core end-to-end tests. Uses srt_genclip and srt_probe binaries from the
// build directory via QProcess, and the sr_core library directly.
#include "core/Exporter.h"
#include "core/Project.h"
#include "core/VideoDecoder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTest>

using namespace sr;

class CoreTest : public QObject {
    Q_OBJECT
    QString m_tmp;
    QString m_bin;

    bool runTool(const QString& name, const QStringList& args, int* exitCode = nullptr)
    {
        QProcess p;
        p.start(QDir(m_bin).filePath(name), args);
        if (!p.waitForFinished(60000)) return false;
        if (exitCode) *exitCode = p.exitCode();
        return p.exitStatus() == QProcess::NormalExit;
    }

    QString genClip(const QString& name, const QStringList& extra = {})
    {
        const QString out = QDir(m_tmp).filePath(name);
        int code = -1;
        if (!runTool("srt_genclip", QStringList{"-o", out, "-n", "24"} << extra, &code)
            || code != 0)
            return {};
        return QFileInfo::exists(out) ? out : QString();
    }

    QJsonObject probeJson(const QString& mode, const QString& file,
                          const QStringList& extra = {}, bool* ok = nullptr)
    {
        QProcess p;
        p.start(QDir(m_bin).filePath("srt_probe"),
                QStringList{mode, file} << extra);
        p.waitForFinished(60000);
        QJsonParseError pe{};
        const auto doc = QJsonDocument::fromJson(p.readAllStandardOutput(), &pe);
        if (ok) *ok = (pe.error == QJsonParseError::NoError && doc.isObject());
        return doc.object();
    }

private slots:
    void initTestCase()
    {
        m_bin = QCoreApplication::applicationDirPath();
        m_tmp = QDir::tempPath() + "/srt_test_"
               + QString::number(QCoreApplication::applicationPid());
        QVERIFY(QDir().mkpath(m_tmp));
    }

    void syntheticClipHasUniqueFramesAndVfr()
    {
        const QString clip = genClip("vfr.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QString err;
        QVERIFY2(dec.open(clip, &err), qPrintable(err));
        QVERIFY2(dec.buildIndex(nullptr, &err), qPrintable(err));
        QCOMPARE(dec.frameCount(), 24);

        QSet<int64_t> durations;
        for (int i = 1; i < dec.frameCount(); ++i) {
            QVERIFY(dec.index()[i].ptsUs > dec.index()[i - 1].ptsUs);
            durations.insert(dec.index()[i - 1].durationUs);
        }
        QVERIFY2(durations.size() > 1, "expected non-uniform frame durations (VFR)");
        bool ok = false;
        const auto info = probeJson("info", clip, {}, &ok);
        QVERIFY(ok);
        QCOMPARE(info.value("isVariableFrameRate").toBool(), true);
        QCOMPARE(info.value("frameCount").toInt(), 24);
    }

    void exactSeekingLandsOnRequestedFrameNotKeyframe()
    {
        const QString clip = genClip("seek.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QString err;
        QVERIFY(dec.open(clip, &err));
        QVERIFY(dec.buildIndex(nullptr, &err));

        for (int target : {1, 2, 7, 13, 17, 23}) {
            DecodedFrame f;
            QVERIFY2(dec.frameAt(target, f, &err),
                     qPrintable(QString("frame %1: %2").arg(target).arg(err)));
            QCOMPARE(f.index, target);
            QCOMPARE(f.ptsUs, dec.index()[target].ptsUs);
            bool ok = false;
            const auto j = probeJson("readid", clip, {QString::number(target)}, &ok);
            QVERIFY(ok);
            QCOMPARE(j.value("requestedIndex").toInt(), target);
            QCOMPARE(j.value("embeddedFrameId").toInt(), target);
        }
    }

    void repeatExportStartEndDurationAndTail()
    {
        const QString clip = genClip("rep.mkv", {"--audio"});
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QString err;
        QVERIFY(dec.open(clip, &err));
        QVERIFY(dec.buildIndex(nullptr, &err));

        ExportSettings s;
        s.outputPath = QDir(m_tmp).filePath("rep_out.mkv");
        s.startFrame = 3;
        s.endFrame = 9;
        s.speed = 0.5;
        s.slowmoMode = SlowmoMode::Repeat;
        s.audioMode = AudioMode::Mute;
        s.outputFrameRate = 60;

        Exporter ex(dec);
        ExportReport rep;
        QVERIFY2(ex.run(s, rep), qPrintable(rep.error));

        QCOMPARE(rep.startFrame, 3);
        QCOMPARE(rep.endFrame, 9);
        QSet<int> sourceFramesUsed;
        for (const auto& p : rep.frames) sourceFramesUsed.insert(p.sourceFrameA);
        QCOMPARE(sourceFramesUsed, QSet<int>({3, 4, 5, 6, 7, 8, 9}));
        QVERIFY(rep.originalFrameCount > 0);
        QVERIFY(rep.generatedFrameCount > 0);

        const int64_t span = dec.index()[9].ptsUs + dec.index()[9].durationUs
                             - dec.index()[3].ptsUs;
        QVERIFY(qAbs(rep.outputDurationUs - span * 2) <= 1'000'000 / 60);
        QVERIFY(qAbs(rep.expectedDurationUs - span * 2) <= 2);

        bool ok = false;
        const auto v = probeJson("verify", rep.outputPath, {}, &ok);
        QVERIFY(ok);
        QCOMPARE(v.value("ok").toBool(), true);
        QCOMPARE(v.value("decodedFrameCount").toInt(), rep.outputFrameCount);
        QCOMPARE(v.value("hasAudio").toBool(), false);
        QCOMPARE(v.value("truncatedTail").toBool(), false);

        QFile sf(rep.sidecarPath);
        QVERIFY(sf.open(QIODevice::ReadOnly));
        const auto meta = QJsonDocument::fromJson(sf.readAll()).object();
        QCOMPARE(meta.value("originalFrames").toInt(), rep.originalFrameCount);
        QCOMPARE(meta.value("generatedFrames").toInt(), rep.generatedFrameCount);
    }

    void interpolateExportMarksGeneratedFrames()
    {
        const QString clip = genClip("interp.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QString err;
        QVERIFY(dec.open(clip, &err));
        QVERIFY(dec.buildIndex(nullptr, &err));

        ExportSettings s;
        s.outputPath = QDir(m_tmp).filePath("interp_out.mkv");
        s.startFrame = 2;
        s.endFrame = 6;
        s.speed = 0.25;
        s.slowmoMode = SlowmoMode::Interpolate;
        s.audioMode = AudioMode::Mute;
        s.outputFrameRate = 60;
        Exporter ex(dec);
        ExportReport rep;
        QVERIFY2(ex.run(s, rep), qPrintable(rep.error));
        bool sawInterp = false;
        for (const auto& p : rep.frames) {
            if (p.kind == "interpolated") {
                sawInterp = true;
                QVERIFY(p.generated);
                QVERIFY(p.sourceFrameB >= 0);
                QVERIFY(p.blendWeight > 0.0 && p.blendWeight <= 1.0);
            }
        }
        QVERIFY(sawInterp);
        bool ok = false;
        const auto v = probeJson("verify", rep.outputPath, {}, &ok);
        QVERIFY(ok);
        QCOMPARE(v.value("ok").toBool(), true);
    }

    void audioModesProduceStatedStreams()
    {
        const QString clip = genClip("aud.mkv", {"--audio"});
        QVERIFY(!clip.isEmpty());
        for (auto mode : {AudioMode::Atempo, AudioMode::Drop}) {
            VideoDecoder dec;
            QVERIFY(dec.open(clip));
            QVERIFY(dec.buildIndex());
            ExportSettings s;
            s.outputPath = QDir(m_tmp).filePath(
                mode == AudioMode::Atempo ? "aud_atempo.mkv" : "aud_drop.mkv");
            s.startFrame = 0; s.endFrame = 11; s.speed = 0.5;
            s.slowmoMode = SlowmoMode::Repeat;
            s.audioMode = mode;
            s.outputFrameRate = 60;
            Exporter ex(dec);
            ExportReport rep;
            QVERIFY2(ex.run(s, rep), qPrintable(rep.error));
            QVERIFY(rep.hasAudio);
            bool ok = false;
            const auto v = probeJson("info", rep.outputPath, {}, &ok);
            QVERIFY(ok);
            QCOMPARE(v.value("hasAudio").toBool(), true);
        }

        const QString silent = genClip("silent.mkv");
        QVERIFY(!silent.isEmpty());
        VideoDecoder dec;
        QVERIFY(dec.open(silent));
        QVERIFY(dec.buildIndex());
        QCOMPARE(dec.summary().audioStreamIndex, -1);
        ExportSettings s;
        s.outputPath = QDir(m_tmp).filePath("from_silent.mkv");
        s.startFrame = 0; s.endFrame = 5; s.speed = 0.5;
        s.audioMode = AudioMode::Atempo;
        Exporter ex(dec);
        ExportReport rep;
        QVERIFY(ex.run(s, rep));
        QVERIFY(!rep.hasAudio);
        bool ok = false;
        QCOMPARE(probeJson("info", rep.outputPath, {}, &ok).value("hasAudio").toBool(), false);
        QVERIFY(ok);
    }

    void corruptTailIsReportedAndGoodFramesRemainUsable()
    {
        const QString clip = genClip("broken.mkv", {"--corrupt-tail"});
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QString err;
        QVERIFY(dec.open(clip, &err));
        QVERIFY(dec.buildIndex(nullptr, &err));
        QVERIFY(dec.summary().truncatedTail);
        QVERIFY(dec.frameCount() > 0 && dec.frameCount() < 24);
        DecodedFrame f;
        QVERIFY(dec.frameAt(0, f, &err));
        QCOMPARE(f.index, 0);
        bool ok = false;
        const auto info = probeJson("info", clip, {}, &ok);
        QVERIFY(ok);
        QCOMPARE(info.value("truncatedTail").toBool(), true);
    }

    void projectMarkersGoPendingOnMaterialReplacement()
    {
        const QString a = genClip("mat_a.mkv");
        const QString b = genClip("mat_b.mkv");
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        Project p;
        {
            VideoDecoder d;
            QVERIFY(d.open(a));
            QVERIFY(d.buildIndex());
            p.source() = d.summary();
            p.addMarker(5, d.index()[5].ptsUs, MarkerPhase::Takeoff);
            p.addMarker(10, d.index()[10].ptsUs, MarkerPhase::Landing);
        }
        const QString proj = QDir(m_tmp).filePath("p.srtproj");
        QVERIFY(p.save(proj));

        QFile::remove(a);
        QVERIFY(QFile::copy(b, a));

        Project loaded;
        QVERIFY(loaded.load(proj));
        QString detail;
        QCOMPARE(loaded.verifySource(&detail), false);
        QVERIFY(!detail.isEmpty());
        for (const auto& m : loaded.markers())
            QCOMPARE(m.state, MarkerState::Pending);

        VideoDecoder d;
        QVERIFY(d.open(a));
        QVERIFY(d.buildIndex());
        loaded.replaceSource(d.summary());
        QCOMPARE(loaded.markers().first().state, MarkerState::Pending);
        auto& m0 = loaded.markers()[0];
        m0.ptsUs = d.index()[m0.frameIndex].ptsUs;
        m0.state = MarkerState::Active;
        QCOMPARE(loaded.markers().first().state, MarkerState::Active);
    }

    void markersStoreExactPtsNotIndexDerivedTime()
    {
        const QString clip = genClip("pts.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder d;
        QVERIFY(d.open(clip));
        QVERIFY(d.buildIndex());
        Project p;
        p.source() = d.summary();
        p.addMarker(4, d.index()[4].ptsUs, MarkerPhase::Air);
        const auto m = p.markers().first();
        QCOMPARE(m.ptsUs, d.index()[4].ptsUs);
        const double naive = 4'000'000.0 / d.summary().avgFrameRate;
        QVERIFY(qAbs(double(m.ptsUs) - naive) > 1.0);
    }
};

QTEST_MAIN(CoreTest)
#include "test_core.moc"
