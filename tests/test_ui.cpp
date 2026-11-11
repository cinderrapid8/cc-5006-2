#include "ui/MainWindow.h"
#include "core/VideoDecoder.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
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
};

QTEST_MAIN(UiTest)
#include "test_ui.moc"
