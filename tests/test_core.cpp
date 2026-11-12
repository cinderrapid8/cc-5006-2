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

    // ------------------------------------------------------------------
    // Segmented speed curve acceptance cases.
    // ------------------------------------------------------------------

    void constantCurveIsExactLegacySpecialCase()
    {
        const QString clip = genClip("legacy.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QVERIFY(dec.open(clip));
        QVERIFY(dec.buildIndex());

        ExportSettings legacy;
        legacy.outputPath = QDir(m_tmp).filePath("legacy_const.mkv");
        legacy.startFrame = 2;
        legacy.endFrame = 17;
        legacy.speed = 0.25;
        legacy.hasCurve = false;
        legacy.slowmoMode = SlowmoMode::Repeat;
        legacy.outputFrameRate = 60;

        ExportSettings curved = legacy;
        curved.outputPath = QDir(m_tmp).filePath("legacy_curve.mkv");
        curved.hasCurve = true;
        const int64_t span = dec.index()[17].ptsUs + dec.index()[17].durationUs
                             - dec.index()[2].ptsUs;
        curved.curve = SpeedCurve::constant(0.25, span);

        Exporter ex1(dec);
        ExportReport r1;
        QVERIFY2(ex1.run(legacy, r1), qPrintable(r1.error));
        Exporter ex2(dec);
        ExportReport r2;
        QVERIFY2(ex2.run(curved, r2), qPrintable(r2.error));

        // Same duration, same count and identical per-frame provenance.
        QCOMPARE(r1.outputDurationUs, r2.outputDurationUs);
        QCOMPARE(r1.frames.size(), r2.frames.size());
        for (int i = 0; i < r1.frames.size(); ++i) {
            QCOMPARE(r1.frames[i].outputPtsUs, r2.frames[i].outputPtsUs);
            QCOMPARE(r1.frames[i].sourceFrameA, r2.frames[i].sourceFrameA);
            QCOMPARE(r1.frames[i].sourceFrameB, r2.frames[i].sourceFrameB);
            QCOMPARE(r1.frames[i].kind, r2.frames[i].kind);
            QCOMPARE(r1.frames[i].sourceTimeUs, r2.frames[i].sourceTimeUs);
        }
    }

    void curveRejectsNegativeZeroAndInfiniteSpeeds()
    {
        SpeedCurve c;
        c.setKnots({{0, 0.5, 0}, {500'000, -0.1, 0}, {1'000'000, 0.5, 0}});
        QString reason;
        QVERIFY(!c.validate(1'000'000, &reason));
        QVERIFY(reason.contains("rejected speed"));

        // Zero speed (infinite output duration) is a stall, not a freeze:
        // explicit holds must be declared via freezeDurationUs instead.
        SpeedCurve zero;
        zero.setKnots({{0, 0.5, 0}, {500'000, 0.0, 0}, {1'000'000, 0.5, 0}});
        QVERIFY(!zero.validate(1'000'000, &reason));

        // Coincident control points are refused rather than producing a
        // zero-width segment whose slope is undefined.
        SpeedCurve coincide;
        coincide.setKnots({{0, 0.5, 0}, {500'000, 0.2, 0},
                           {500'000, 0.8, 0}, {1'000'000, 0.5, 0}});
        QVERIFY(!coincide.validate(1'000'000, &reason));
        QVERIFY(reason.contains("coincide"));

        // Curve not anchored on the interval ends is rejected.
        SpeedCurve unanchored;
        unanchored.setKnots({{100, 0.5, 0}, {1'000'000, 0.5, 0}});
        QVERIFY(!unanchored.validate(1'000'000));
    }

    void mappingStaysMonotoneOutsideExplicitFreezes()
    {
        // Entry 1.0 -> abrupt slowdown 0.05 in a narrow band -> hold ->
        // acceleration 0.8 exit, all inside one 1 ms source span.
        SpeedCurve c;
        c.setKnots({{0, 1.0, 0},
                    {200'000, 0.05, 0},
                    {260'000, 0.8, 120'000},   // narrow 60 us slow region + hold
                    {1'000'000, 0.8, 0}});
        QString reason;
        QVERIFY2(c.validate(1'000'000, &reason), qPrintable(reason));

        int64_t lastOut = -1;
        for (int64_t u = 0; u <= 1'000'000; u += 100) {
            const int64_t s = c.sourceToOutput(u);
            QVERIFY2(s >= lastOut, "sourceToOutput must be monotone");
            lastOut = s;
        }
        int64_t lastSrc = -1;
        const int64_t total = c.outputDurationUs();
        for (int64_t s = 0; s < total; s += 100) {
            const int64_t u = c.outputToSource(s);
            QVERIFY2(u >= lastSrc, "outputToSource must be monotone");
            lastSrc = u;
        }

        // The explicit freeze holds one source position over 120 ms of output.
        const auto segs = c.inverseSegments();
        bool sawFreeze = false;
        for (const auto& seg : segs) {
            if (seg.speed == 0.0) {
                sawFreeze = true;
                QCOMPARE(seg.srcStartUs, seg.srcEndUs);
                QCOMPARE(seg.outEndUs - seg.outStartUs, 120'000);
            }
        }
        QVERIFY(sawFreeze);
    }

    void narrowDecelerationBandIsSampledFromRealVfrTimestamps()
    {
        const QString clip = genClip("narrow.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QVERIFY(dec.open(clip));
        QVERIFY(dec.buildIndex());
        const auto& idx = dec.index();

        // Anchor the slow region between two real source frames with a very
        // narrow gap, then check every output frame against the mapping by
        // hand: this is the "极窄减速区间" acceptance case.
        const int startFrame = 0;
        const int endFrame = dec.frameCount() - 1;
        const int64_t spanStart = idx[startFrame].ptsUs;
        const int64_t span = idx[endFrame].ptsUs + idx[endFrame].durationUs
                             - spanStart;
        const int64_t slowA = idx[5].ptsUs - spanStart;
        const int64_t slowB = idx[6].ptsUs - spanStart; // one real VFR gap wide
        QVERIFY(slowB > slowA);

        SpeedCurve curve;
        curve.setKnots({{0, 1.0, 0},
                        {slowA, 0.02, 0},
                        {slowB, 1.0, 0},
                        {span, 1.0, 0}});
        QString reason;
        QVERIFY2(curve.validate(span, &reason), qPrintable(reason));

        FramePlan plan = FramePlanner::plan(idx, startFrame, endFrame, curve,
                                            120, SlowmoMode::Repeat, &reason);
        QVERIFY2(plan.ok, qPrintable(plan.error));

        // Re-derive each output frame independently from the same mapping and
        // confirm the planner never used a different/approximate rule.
        int64_t prevSrcRel = -1;
        for (const auto& pf : plan.frames) {
            const int64_t expectRel = curve.outputToSource(pf.prov.outputPtsUs);
            const int64_t gotRel = pf.prov.sourceTimeUs - spanStart;
            QVERIFY(qAbs(gotRel - expectRel) <= 1);
            QVERIFY(gotRel >= prevSrcRel); // monotone even inside the narrow band
            prevSrcRel = gotRel;
            QVERIFY(pf.prov.sourceFrameA >= startFrame);
            QVERIFY(pf.prov.sourceFrameA <= endFrame);
            QVERIFY(!pf.prov.kind.isEmpty());
        }
        QCOMPARE(plan.outputDurationUs, curve.outputDurationUs());

        // The narrow band must actually contain several output frames that
        // all resolve to source frame 5's position (the slow side).
        int bandFrames = 0;
        for (const auto& pf : plan.frames) {
            const int64_t rel = pf.prov.sourceTimeUs - spanStart;
            if (rel >= slowA && rel < slowB) ++bandFrames;
        }
        QVERIFY2(bandFrames >= 3,
                 qPrintable(QString("narrow band produced %1 frames").arg(bandFrames)));
    }

    void nearlyCoincidentControlPointsRemainFiniteAndOrdered()
    {
        SpeedCurve c;
        // 1 microsecond apart — the minimum legal gap — at speed 0.01.
        c.setKnots({{0, 0.5, 0},
                    {499'999, 0.01, 0},
                    {500'000, 0.5, 0},
                    {1'000'000, 0.5, 0}});
        QString reason;
        QVERIFY2(c.validate(1'000'000, &reason), qPrintable(reason));
        const auto segs = c.inverseSegments();
        for (int i = 1; i < segs.size(); ++i)
            QVERIFY(segs[i].outStartUs >= segs[i - 1].outStartUs);
        QVERIFY(c.outputDurationUs() > 0);
        QVERIFY(c.outputDurationUs() < SpeedCurve::maxOutputDurationUs);

        // A gap below the minimum must be refused instead of collapsing the
        // two knots into an undefined segment.
        SpeedCurve bad;
        bad.setKnots({{0, 0.5, 0}, {500'000, 0.01, 0},
                      {500'000, 0.5, 0}, {1'000'000, 0.5, 0}});
        QVERIFY(!bad.validate(1'000'000));
    }

    void freezeSegmentMarksFramesAndKeepsOriginRecord()
    {
        const QString clip = genClip("freeze.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QVERIFY(dec.open(clip));
        QVERIFY(dec.buildIndex());
        const auto& idx = dec.index();
        const int64_t span = idx.last().ptsUs + idx.last().durationUs
                             - idx.first().ptsUs;
        const int64_t holdAt = idx[8].ptsUs - idx.first().ptsUs;

        SpeedCurve curve;
        curve.setKnots({{0, 0.5, 0},
                        {holdAt, 0.5, 200'000},
                        {span, 0.5, 0}});
        ExportSettings s;
        s.outputPath = QDir(m_tmp).filePath("freeze_out.mkv");
        s.startFrame = 0;
        s.endFrame = dec.frameCount() - 1;
        s.hasCurve = true;
        s.curve = curve;
        s.slowmoMode = SlowmoMode::Repeat;
        s.audioMode = AudioMode::Mute;
        s.outputFrameRate = 60;
        Exporter ex(dec);
        ExportReport rep;
        QVERIFY2(ex.run(s, rep), qPrintable(rep.error));
        QVERIFY(rep.freezeFrameCount > 0);

        // Every freeze frame shows the same held source frame and records it.
        QSet<int> frozenSources;
        for (const auto& p : rep.frames)
            if (p.kind == "freeze") {
                frozenSources.insert(p.sourceFrameA);
                QCOMPARE(p.sourceFrameB, -1);
                QVERIFY(p.generated);
            }
        QCOMPARE(frozenSources.size(), 1);

        bool ok = false;
        const auto v = probeJson("verify", rep.outputPath, {}, &ok);
        QVERIFY(ok);
        QCOMPARE(v.value("ok").toBool(), true);

        // Sidecar keeps the curve and per-frame segment provenance.
        QFile sf(rep.sidecarPath);
        QVERIFY(sf.open(QIODevice::ReadOnly));
        const auto meta = QJsonDocument::fromJson(sf.readAll()).object();
        QCOMPARE(meta.value("hasCurve").toBool(), true);
        QCOMPARE(meta.value("freezeFrames").toInt(), rep.freezeFrameCount);
        QVERIFY(meta.value("speedCurve").toObject().value("knots").toArray().size() >= 2);
    }

    void curvePreviewPlanEqualsExportedFrameSelection()
    {
        const QString clip = genClip("sameplan.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QVERIFY(dec.open(clip));
        QVERIFY(dec.buildIndex());
        const auto& idx = dec.index();
        const int64_t span = idx[15].ptsUs + idx[15].durationUs - idx[3].ptsUs;
        SpeedCurve curve;
        curve.setKnots({{0, 1.0, 0}, {span / 3, 0.1, 0},
                        {2 * span / 3, 0.6, 50'000}, {span, 1.0, 0}});

        // The "preview cache" plan: provenance only, same call the UI makes.
        FramePlan preview = FramePlanner::plan(idx, 3, 15, curve, 60,
                                               SlowmoMode::Interpolate);
        QVERIFY(preview.ok);

        ExportSettings s;
        s.outputPath = QDir(m_tmp).filePath("sameplan_out.mkv");
        s.startFrame = 3;
        s.endFrame = 15;
        s.hasCurve = true;
        s.curve = curve;
        s.slowmoMode = SlowmoMode::Interpolate;
        s.outputFrameRate = 60;
        Exporter ex(dec);
        ExportReport rep;
        QVERIFY2(ex.run(s, rep), qPrintable(rep.error));

        QCOMPARE(preview.frames.size(), rep.frames.size());
        for (int i = 0; i < preview.frames.size(); ++i) {
            QCOMPARE(preview.frames[i].prov.sourceFrameA,
                     rep.frames[i].sourceFrameA);
            QCOMPARE(preview.frames[i].prov.sourceFrameB,
                     rep.frames[i].sourceFrameB);
            QCOMPARE(preview.frames[i].prov.kind, rep.frames[i].kind);
            QCOMPARE(preview.frames[i].prov.outputPtsUs,
                     rep.frames[i].outputPtsUs);
        }
    }

    void segmentedAtempoAudioUsesRealFilterAndFreezePolicy()
    {
        const QString clip = genClip("seg_audio.mkv", {"--audio"});
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QVERIFY(dec.open(clip));
        QVERIFY(dec.buildIndex());
        const auto& idx = dec.index();
        const int64_t span = idx.last().ptsUs + idx.last().durationUs
                             - idx.first().ptsUs;
        SpeedCurve curve;
        curve.setKnots({{0, 1.0, 0},
                        {span / 3, 0.25, 0},
                        {span / 2, 0.25, 150'000}, // freeze inside the slow part
                        {2 * span / 3, 0.8, 0},
                        {span, 1.0, 0}});

        for (auto policy : {AudioFreezePolicy::Silence, AudioFreezePolicy::HoldLast}) {
            ExportSettings s;
            s.outputPath = QDir(m_tmp).filePath(
                policy == AudioFreezePolicy::Silence ? "seg_silence.mkv"
                                                     : "seg_hold.mkv");
            s.startFrame = 0;
            s.endFrame = dec.frameCount() - 1;
            s.hasCurve = true;
            s.curve = curve;
            s.slowmoMode = SlowmoMode::Repeat;
            s.audioMode = AudioMode::Atempo;
            s.audioFreezePolicy = policy;
            s.outputFrameRate = 60;
            Exporter ex(dec);
            ExportReport rep;
            QVERIFY2(ex.run(s, rep), qPrintable(rep.error));
            QVERIFY(rep.hasAudio);
            bool ok = false;
            const auto info = probeJson("info", rep.outputPath, {}, &ok);
            QVERIFY(ok);
            QCOMPARE(info.value("hasAudio").toBool(), true);
            QFile sf(rep.sidecarPath);
            QVERIFY(sf.open(QIODevice::ReadOnly));
            const auto meta = QJsonDocument::fromJson(sf.readAll()).object();
            QCOMPARE(meta.value("audioMode").toString(),
                     QStringLiteral("atempo filter (pitch preserved)"));
            QCOMPARE(meta.value("audioFreezePolicy").toString(),
                     audioFreezePolicyName(policy));
        }
    }

    void savedProjectReopensWithIdenticalCurveTiming()
    {
        const QString clip = genClip("proj_curve.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QVERIFY(dec.open(clip));
        QVERIFY(dec.buildIndex());
        const auto& idx = dec.index();
        const int64_t span = idx[12].ptsUs + idx[12].durationUs - idx[2].ptsUs;

        SpeedCurve curve;
        curve.setKnots({{0, 1.0, 0}, {span / 2, 0.1, 80'000}, {span, 0.7, 0}});
        Project p;
        p.source() = dec.summary();
        ExportConfig cfg;
        cfg.startFrame = 2;
        cfg.endFrame = 12;
        cfg.hasCurve = true;
        cfg.curve = curve;
        cfg.speed = 0.5;
        cfg.outputFrameRate = 60;
        p.setExportConfig(cfg);
        const QString path = QDir(m_tmp).filePath("curve.srtproj");
        QVERIFY(p.save(path));

        Project reopened;
        QVERIFY(reopened.load(path));
        QVERIFY(reopened.hasExportConfig());
        QCOMPARE(reopened.exportConfig().hasCurve, true);
        const auto& knots = reopened.exportConfig().curve.knots();
        QCOMPARE(knots.size(), curve.knots().size());
        for (int i = 0; i < knots.size(); ++i) {
            QCOMPARE(knots[i].timeUs, curve.knots()[i].timeUs);
            QCOMPARE(knots[i].speed, curve.knots()[i].speed);
            QCOMPARE(knots[i].freezeDurationUs, curve.knots()[i].freezeDurationUs);
        }

        // Render from the reopened project: identical duration/frame count.
        ExportSettings s;
        s.outputPath = QDir(m_tmp).filePath("reopened_curve.mkv");
        s.startFrame = reopened.exportConfig().startFrame;
        s.endFrame = reopened.exportConfig().endFrame;
        s.hasCurve = true;
        s.curve = reopened.exportConfig().curve;
        s.outputFrameRate = reopened.exportConfig().outputFrameRate;
        Exporter ex(dec);
        ExportReport rep;
        QVERIFY2(ex.run(s, rep), qPrintable(rep.error));
        // Rendered duration is the mapping length quantised to one output
        // frame step; the mapping duration itself is reported exactly.
        QCOMPARE(rep.expectedDurationUs, curve.outputDurationUs());
        QVERIFY(qAbs(rep.outputDurationUs - curve.outputDurationUs())
                <= 1'000'000 / cfg.outputFrameRate);
    }

    void legacyV1ProjectLoadsAndRendersUnchanged()
    {
        // Hand-write a v1 project file (no exportConfig/curve fields).
        const QString clip = genClip("v1.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QVERIFY(dec.open(clip));
        QVERIFY(dec.buildIndex());
        Project p;
        p.source() = dec.summary();
        p.addMarker(3, dec.index()[3].ptsUs, MarkerPhase::Takeoff);
        const QString v1 = QDir(m_tmp).filePath("v1.srtproj");
        QVERIFY(p.save(v1));
        {
            // Downgrade the written file to formatVersion 1 and strip the
            // v2-only exportConfig block, simulating an old saved project.
            QFile f(v1);
            QVERIFY(f.open(QIODevice::ReadOnly));
            auto root = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            root["formatVersion"] = Project::legacyFileFormatVersion;
            root.remove("exportConfig");
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        }
        Project loaded;
        QVERIFY(loaded.load(v1));
        QVERIFY(!loaded.hasExportConfig());
        QCOMPARE(loaded.markers().size(), 1);

        // The legacy constant-speed export must keep its historical timing.
        ExportSettings s;
        s.outputPath = QDir(m_tmp).filePath("v1_out.mkv");
        s.startFrame = 0;
        s.endFrame = 10;
        s.hasCurve = false;
        s.speed = 0.5;
        s.slowmoMode = SlowmoMode::Repeat;
        s.outputFrameRate = 60;
        Exporter ex(dec);
        ExportReport rep;
        QVERIFY2(ex.run(s, rep), qPrintable(rep.error));
        const int64_t span = dec.index()[10].ptsUs + dec.index()[10].durationUs
                             - dec.index()[0].ptsUs;
        QVERIFY(qAbs(rep.expectedDurationUs - span * 2) <= 2);
        QCOMPARE(rep.curve.isConstant(), true);
    }

    void cliAcceptanceNarrowBandMappingChecksFrameByFrame()
    {
        // Drives the same srt_curve_demo + srt_probe mapping path documented
        // in docs/speed-curve-acceptance.md.
        const QString clip = genClip("cli_vfr.mkv", {"--audio"});
        QVERIFY(!clip.isEmpty());
        const QString out = QDir(m_tmp).filePath("cli_narrow.mkv");
        const QString sidecar = out + ".frames.json";
        int code = -1;
        QVERIFY(runTool("srt_curve_demo",
                        {"--input", clip, "--output", out, "--fps", "120",
                         "--slow-a", "5", "--slow-b", "6",
                         "--freeze-us", "150000", "--audio-mode", "atempo",
                         "--freeze-audio", "hold"}, &code) && code == 0);

        // 1) Container verification: PTS grid matches the sidecar exactly.
        bool ok = false;
        const auto v = probeJson("verify", out, {}, &ok);
        QVERIFY(ok);
        QCOMPARE(v.value("ok").toBool(), true);
        QCOMPARE(v.value("ptsMismatches").toInt(), 0);
        QCOMPARE(v.value("hasAudio").toBool(), true);

        // 2) Mapping dump: monotone source time, correct segment structure.
        const auto mp = probeJson("mapping", sidecar, {}, &ok);
        QVERIFY(ok);
        QCOMPARE(mp.value("valid").toBool(), true);
        QCOMPARE(mp.value("sourceTimeMonotone").toBool(), true);
        const auto segs = mp.value("segments").toArray();
        QCOMPARE(segs.size(), 4); // entry, narrow slowdown, freeze, exit
        bool sawFreeze = false;
        for (const auto& sv : segs) {
            const auto so = sv.toObject();
            if (so.value("speed").toDouble() == 0.0) {
                sawFreeze = true;
                QCOMPARE(so.value("srcStartUs").toVariant().toLongLong(),
                         so.value("srcEndUs").toVariant().toLongLong());
            }
        }
        QVERIFY(sawFreeze);

        // 3) The one-frame-gap narrow band must contain many output frames,
        //    all resolving by real PTS inside the band.
        VideoDecoder dec;
        QVERIFY(dec.open(clip));
        QVERIFY(dec.buildIndex());
        const int64_t bandA = dec.index()[5].ptsUs - dec.index()[0].ptsUs;
        const int64_t bandB = dec.index()[6].ptsUs - dec.index()[0].ptsUs;
        int bandFrames = 0;
        for (const auto& fv : mp.value("frames").toArray()) {
            const auto fo = fv.toObject();
            const int64_t srcRel = fo.value("sourceTimeUs").toVariant().toLongLong()
                                   - dec.index()[0].ptsUs;
            if (srcRel >= bandA && srcRel < bandB) ++bandFrames;
        }
        QVERIFY2(bandFrames >= 100,
                 qPrintable(QString("narrow band only produced %1 frames").arg(bandFrames)));

        // 4) Point query at an arbitrary output position round-trips.
        const auto q = probeJson("mapping", sidecar, {"--at-us", "500000"}, &ok);
        QVERIFY(ok);
        QVERIFY(q.contains("querySourceUs"));
        QVERIFY(q.value("querySourceUs").toVariant().toLongLong() >= 0);
    }
};

QTEST_MAIN(CoreTest)
#include "test_core.moc"
