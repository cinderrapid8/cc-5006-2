#include "ui/MainWindow.h"
#include "ui/CurveEditor.h"
#include "ui/ExportDialog.h"
#include "core/VideoDecoder.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTest>
#include <QTimer>

using namespace sr;

class UiTest : public QObject {
    Q_OBJECT
    QString m_tmp;
    QString m_bin;

    QString gen(const QString& name)
    {
        const QString out = QDir(m_tmp).filePath(name);
        QProcess p;
        p.start(QDir(m_bin).filePath("srt_genclip"), {"-o", out, "-n", "18"});
        if (!p.waitForFinished(60000) || p.exitCode() != 0) return {};
        return out;
    }

    static QPushButton* button(MainWindow& w, const QString& part)
    {
        const auto buttons = w.findChildren<QPushButton*>();
        for (auto* b : buttons)
            if (b->text().contains(part)) return b;
        return nullptr;
    }

    static QList<QLabel*> labelsContaining(MainWindow& w, const QString& part)
    {
        QList<QLabel*> hits;
        for (auto* l : w.findChildren<QLabel*>())
            if (l->text().contains(part)) hits.append(l);
        return hits;
    }

private slots:
    void initTestCase()
    {
        m_bin = QCoreApplication::applicationDirPath();
        m_tmp = QDir::tempPath() + "/srt_uitest_"
               + QString::number(QCoreApplication::applicationPid());
        QVERIFY(QDir().mkpath(m_tmp));
    }

    void windowConstructsAndStartsDisabled()
    {
        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* spin = w.findChild<QSpinBox*>();
        QVERIFY(spin);
        QCOMPARE(spin->isEnabled(), false);
        auto* exportBtn = button(w, "Export");
        QVERIFY(exportBtn);
        QCOMPARE(exportBtn->isEnabled(), false);
    }

    void loadShowsRealPtsAndMarkerFlowWorks()
    {
        const QString clip = gen("ui.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QVERIFY(dec.open(clip));
        QVERIFY(dec.buildIndex());
        const int count = dec.frameCount();
        const int64_t pts7 = dec.index()[7].ptsUs;

        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openSourceFile(clip));
        QVERIFY(w.findChild<QSpinBox*>()->isEnabled());
        QCOMPARE(w.findChild<QSpinBox*>()->maximum(), count - 1);

        auto* spin = w.findChild<QSpinBox*>();
        spin->setValue(7);
        QTRY_COMPARE(spin->value(), 7);
        QCOMPARE(w.currentFrame(), 7);
        QVERIFY(!labelsContaining(w, QString::number(static_cast<qint64>(pts7))).isEmpty());
        QVERIFY(!labelsContaining(w, "Average FPS").isEmpty());

        auto* takeoff = button(w, "takeoff");
        QVERIFY(takeoff);
        QTest::mouseClick(takeoff, Qt::LeftButton);
        QCOMPARE(w.markerCount(), 1);
        auto* list = w.findChild<QListWidget*>();
        QVERIFY(list->item(0)->text().contains("takeoff"));
        QVERIFY(labelsContaining(w, "PENDING").isEmpty());

        // Step transport and add a second marker of another phase.
        spin->setValue(11);
        QTRY_COMPARE(spin->value(), 11);
        auto* landing = button(w, "landing");
        QTest::mouseClick(landing, Qt::LeftButton);
        QCOMPARE(w.markerCount(), 2);

        // Project round-trip through a saved file.
        const QString proj = QDir(m_tmp).filePath("ui.srtproj");
        QVERIFY(w.saveProjectTo(proj));
        QVERIFY(QFileInfo::exists(proj));
    }

    void replacedSourceMarksMarkersPending()
    {
        const QString a = gen("a.mkv");
        const QString b = gen("b.mkv");
        QVERIFY(!a.isEmpty() && !b.isEmpty());

        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openSourceFile(a));

        auto* landing = button(w, "landing");
        QVERIFY(landing);
        QTest::mouseClick(landing, Qt::LeftButton);
        QCOMPARE(w.markerCount(), 1);

        QFile::remove(a);
        QVERIFY(QFile::copy(b, a));
        QString detail;
        QCOMPARE(w.verifySourceNoDialog(&detail), false);
        QVERIFY(!detail.isEmpty());
        QVERIFY(!labelsContaining(w, "PENDING").isEmpty());
        QCOMPARE(w.markerCount(), 1);
    }

    void loadProjectReopensSourceAndKeepsMarkers()
    {
        const QString clip = gen("proj.mkv");
        QVERIFY(!clip.isEmpty());
        const QString projPath = QDir(m_tmp).filePath("roundtrip.srtproj");
        {
            MainWindow w;
            QVERIFY(w.openSourceFile(clip));
            w.findChild<QSpinBox*>()->setValue(5);
            QTest::mouseClick(button(w, "air"), Qt::LeftButton);
            QVERIFY(w.saveProjectTo(projPath));
        }
        MainWindow w2;
        w2.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w2));
        QVERIFY(w2.loadProjectFrom(projPath));
        QCOMPARE(w2.markerCount(), 1);
        QVERIFY(w2.findChild<QSpinBox*>()->isEnabled());
    }

    void curveEditorShowsActualOutputLengthAndRejectsBadCurve()
    {
        CurveEditor editor;
        editor.setSourceSpan(1'000'000);
        editor.resetToConstant(0.5);
        QVERIFY(editor.isValid());
        // Constant 0.5 over 1 s -> 2 s output.
        QCOMPARE(editor.outputDurationUs(), 2'000'000);

        // A segmented curve: enter at 1.0, slow to 0.1 in the middle, hold,
        // then exit at 0.8. The displayed length must match the mapping math.
        SpeedCurve c;
        c.setKnots({{0, 1.0, 0},
                    {250'000, 0.1, 0},
                    {500'000, 0.8, 100'000},
                    {1'000'000, 0.8, 0}});
        editor.setCurve(c);
        QVERIFY(editor.isValid());
        QCOMPARE(editor.outputDurationUs(), c.outputDurationUs());
        QVERIFY(editor.outputDurationUs() > 2'000'000); // slowdown + hold stretch

        // Negative speed must surface as invalid with an explanatory message.
        SpeedCurve bad;
        bad.setKnots({{0, 0.5, 0}, {500'000, -0.2, 0}, {1'000'000, 0.5, 0}});
        editor.setCurve(bad);
        QString reason;
        QVERIFY(!editor.isValid(&reason));
        QVERIFY(reason.contains("rejected speed"));

        // The metrics label reflects the invalid state rather than a length.
        bool sawInvalid = false;
        for (auto* l : editor.findChildren<QLabel*>())
            if (l->text().contains("Invalid curve")) sawInvalid = true;
        QVERIFY(sawInvalid);
        QCOMPARE(editor.outputDurationUs(), -1);

        editor.resetToConstant(0.25);
        QVERIFY(editor.isValid());
        QCOMPARE(editor.outputDurationUs(), 4'000'000);
    }

    void exportDialogPersistsCurveThroughProject()
    {
        const QString clip = gen("dlg.mkv");
        QVERIFY(!clip.isEmpty());
        VideoDecoder dec;
        QVERIFY(dec.open(clip));
        QVERIFY(dec.buildIndex());
        const auto& idx = dec.index();
        const int64_t span = idx.last().ptsUs + idx.last().durationUs
                             - idx.first().ptsUs;

        ExportConfig initial;
        initial.startFrame = 1;
        initial.endFrame = dec.frameCount() - 2;
        initial.hasCurve = true;
        initial.curve.setKnots({{0, 1.0, 0}, {span / 2, 0.1, 60'000},
                                 {span, 0.8, 0}});
        initial.outputFrameRate = 60;
        ExportDialog dlg(dec.frameCount(), dec.summary().audioStreamIndex,
                         dec.index(), &initial);
        auto* useCurve = dlg.findChild<QCheckBox*>();
        QVERIFY(useCurve && useCurve->isChecked());
        const auto got = dlg.exportConfig();
        QCOMPARE(got.hasCurve, true);
        QCOMPARE(got.curve.knots().size(), initial.curve.knots().size());
        QCOMPARE(got.curve.outputDurationUs(), initial.curve.outputDurationUs());

        // Switching the curve off restores the constant special case.
        useCurve->setChecked(false);
        const auto constCfg = dlg.exportConfig();
        QCOMPARE(constCfg.hasCurve, false);
        QVERIFY(constCfg.speed > 0.0 && constCfg.speed <= 1.0);
    }
};

QTEST_MAIN(UiTest)
#include "test_ui.moc"
