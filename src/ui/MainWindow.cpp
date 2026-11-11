#include "ui/MainWindow.h"

#include "ui/ExportDialog.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace sr {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    buildUi();
    setControlsEnabled(false);
    resize(1280, 820);
    setWindowTitle(tr("Sports Action Review Desk — frame-accurate VFR editing"));
}

void MainWindow::buildUi()
{
    auto* root = new QWidget;
    auto* mainLay = new QHBoxLayout(root);

    // Left: video + transport.
    auto* left = new QVBoxLayout;
    m_videoLabel = new QLabel;
    m_videoLabel->setMinimumSize(720, 420);
    m_videoLabel->setAlignment(Qt::AlignCenter);
    m_videoLabel->setStyleSheet("background:#111;color:#ddd;");
    m_videoLabel->setText(tr("No source loaded"));
    left->addWidget(m_videoLabel, 1);

    m_provenanceLabel = new QLabel;
    m_provenanceLabel->setWordWrap(true);
    m_provenanceLabel->setStyleSheet("font-weight:bold;color:#0a5;");
    left->addWidget(m_provenanceLabel);

    m_infoLabel = new QLabel;
    m_infoLabel->setWordWrap(true);
    left->addWidget(m_infoLabel);

    auto* transport = new QHBoxLayout;
    auto* btnFirst = new QPushButton(tr("|< First"));
    auto* btnPrev = new QPushButton(tr("< Prev"));
    m_frameSpin = new QSpinBox;
    m_frameSpin->setMinimum(0);
    auto* btnNext = new QPushButton(tr("Next >"));
    auto* btnLast = new QPushButton(tr("Last >|"));
    transport->addWidget(btnFirst);
    transport->addWidget(btnPrev);
    transport->addWidget(new QLabel(tr("Frame #")));
    transport->addWidget(m_frameSpin);
    transport->addWidget(btnNext);
    transport->addWidget(btnLast);
    left->addLayout(transport);

    auto* markBox = new QGroupBox(tr("Frame markers (bound to exact PTS + source digest)"));
    auto* markLay = new QHBoxLayout(markBox);
    m_btnTakeoff = new QPushButton(tr("Mark takeoff"));
    m_btnAir = new QPushButton(tr("Mark air"));
    m_btnLanding = new QPushButton(tr("Mark landing"));
    auto* btnRemove = new QPushButton(tr("Remove marker"));
    markLay->addWidget(m_btnTakeoff);
    markLay->addWidget(m_btnAir);
    markLay->addWidget(m_btnLanding);
    markLay->addWidget(btnRemove);
    left->addWidget(markBox);

    mainLay->addLayout(left, 3);

    // Right: project, markers, source status.
    auto* right = new QVBoxLayout;
    auto* fileBox = new QGroupBox(tr("Project"));
    auto* fileLay = new QVBoxLayout(fileBox);
    auto* btnOpen = new QPushButton(tr("Open source video…"));
    auto* btnSave = new QPushButton(tr("Save project…"));
    auto* btnSaveAs = new QPushButton(tr("Save project as…"));
    auto* btnLoad = new QPushButton(tr("Load project…"));
    fileLay->addWidget(btnOpen);
    fileLay->addWidget(btnSave);
    fileLay->addWidget(btnSaveAs);
    fileLay->addWidget(btnLoad);
    right->addWidget(fileBox);

    m_sourceStatusLabel = new QLabel;
    m_sourceStatusLabel->setWordWrap(true);
    right->addWidget(m_sourceStatusLabel);

    auto* verLay = new QHBoxLayout;
    m_btnVerify = new QPushButton(tr("Verify source digest"));
    m_btnConfirm = new QPushButton(tr("Confirm selected pending marker"));
    m_btnConfirm->setEnabled(false);
    verLay->addWidget(m_btnVerify);
    verLay->addWidget(m_btnConfirm);
    right->addLayout(verLay);

    m_markerList = new QListWidget;
    auto* markerBox = new QGroupBox(tr("Markers"));
    auto* ml = new QVBoxLayout(markerBox);
    ml->addWidget(m_markerList);
    right->addWidget(markerBox, 1);

    m_btnExport = new QPushButton(tr("Export slow-motion interval…"));
    right->addWidget(m_btnExport);
    mainLay->addLayout(right, 1);

    setCentralWidget(root);

    connect(btnOpen, &QPushButton::clicked, this, &MainWindow::openSource);
    connect(btnSave, &QPushButton::clicked, this, &MainWindow::saveProject);
    connect(btnSaveAs, &QPushButton::clicked, this, &MainWindow::saveProjectAs);
    connect(btnLoad, &QPushButton::clicked, this, &MainWindow::loadProject);
    connect(btnFirst, &QPushButton::clicked, this, &MainWindow::stepFirst);
    connect(btnPrev, &QPushButton::clicked, this, &MainWindow::stepPrev);
    connect(btnNext, &QPushButton::clicked, this, &MainWindow::nextFrame);
    connect(btnLast, &QPushButton::clicked, this, &MainWindow::stepLast);
    connect(m_frameSpin, &QSpinBox::valueChanged, this, [this](int v){ onFrameSpin(v); });
    connect(m_btnTakeoff, &QPushButton::clicked, this, &MainWindow::markTakeoff);
    connect(m_btnAir, &QPushButton::clicked, this, &MainWindow::markAir);
    connect(m_btnLanding, &QPushButton::clicked, this, &MainWindow::markLanding);
    connect(btnRemove, &QPushButton::clicked, this, &MainWindow::removeSelectedMarker);
    connect(m_btnVerify, &QPushButton::clicked, this, &MainWindow::verifyMarkers);
    connect(m_btnConfirm, &QPushButton::clicked, this, &MainWindow::confirmPendingMarker);
    connect(m_markerList, &QListWidget::currentRowChanged, this, &MainWindow::onMarkerRowSelected);
    connect(m_btnExport, &QPushButton::clicked, this, &MainWindow::exportRange);
}

void MainWindow::setControlsEnabled(bool on)
{
    for (auto* w : std::initializer_list<QWidget*>{
             m_btnTakeoff, m_btnAir, m_btnLanding, m_btnExport,
             m_btnVerify, m_frameSpin})
        w->setEnabled(on);
}

void MainWindow::closeSource()
{
    m_decoder.close();
    m_curFrame = -1;
    m_project = Project{};
}

void MainWindow::openSource()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open source video"), QString(),
        tr("Video files (*.mkv *.mp4 *.mov *.avi *.webm);;All files (*)"));
    if (!path.isEmpty()) openSourceFile(path);
}

bool MainWindow::openSourceFile(const QString& path)
{
    auto* prog = new QProgressDialog(tr("Decoding and indexing every frame…"),
                                     QString(), 0, 0, this);
    prog->setWindowModality(Qt::WindowModal);
    prog->setCancelButton(nullptr);
    prog->setMinimumDuration(0);
    QApplication::processEvents();

    QString err;
    if (!m_decoder.open(path, &err)) {
        prog->close();
        QMessageBox::critical(this, tr("Open failed"), err);
        return false;
    }
    if (!m_decoder.buildIndex([&](int n, int) {
            prog->setLabelText(tr("Indexing frame %1").arg(n));
            QApplication::processEvents();
        }, &err)) {
        prog->close();
        QMessageBox::critical(this, tr("Indexing failed"), err);
        closeSource();
        return false;
    }
    prog->close();

    m_project = Project{};
    m_project.source() = m_decoder.summary();
    m_frameSpin->setRange(0, m_decoder.frameCount() - 1);
    setControlsEnabled(true);
    m_sourceStatusLabel->setText(
        tr("Source: %1\n%2x%3, %4 frames, VFR=%5, audio=%6, truncated tail=%7\nSHA256: %8")
            .arg(QFileInfo(path).fileName())
            .arg(m_decoder.summary().width).arg(m_decoder.summary().height)
            .arg(m_decoder.frameCount())
            .arg(m_decoder.index().size() > 1 ? tr("yes (real per-frame PTS)") : tr("unknown"))
            .arg(m_decoder.summary().audioStreamIndex >= 0 ? tr("yes") : tr("no"))
            .arg(m_decoder.summary().truncatedTail ? tr("YES — damaged tail detected")
                                                   : tr("no"))
            .arg(m_decoder.summary().sha256.left(16) + "…"));
    if (m_decoder.summary().truncatedTail)
        m_sourceStatusLabel->setStyleSheet("color:#b33;font-weight:bold;");
    showFrame(0);
    refreshMarkerList();
    return true;
}

int MainWindow::markerCount() const { return m_project.markers().size(); }

void MainWindow::showFrame(int index)
{
    if (!m_decoder.isOpen() || index < 0 || index >= m_decoder.frameCount()) return;
    QString err;
    DecodedFrame fr;
    if (!m_decoder.frameAt(index, fr, &err)) {
        m_videoLabel->setText(tr("Decode error at frame %1: %2").arg(index).arg(err));
        return;
    }
    m_curFrame = index;
    QSignalBlocker b(m_frameSpin);
    m_frameSpin->setValue(index);

    const FrameTime& ft = m_decoder.index()[index];
    m_videoLabel->setPixmap(QPixmap::fromImage(fr.image).scaled(
        m_videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_infoLabel->setText(
        tr("Frame #%1 (unique source frame number)\n"
           "Real PTS: %2 s (%3 µs)   frame duration: %4 µs   keyframe: %5\n"
           "Average FPS %6 is shown for reference only and is never used to compute time.")
            .arg(index)
            .arg(formatTimeUs(ft.ptsUs))
            .arg(static_cast<qint64>(ft.ptsUs))
            .arg(static_cast<qint64>(ft.durationUs))
            .arg(fr.keyFrame ? tr("yes (seek entry only)") : tr("no"))
            .arg(m_decoder.summary().avgFrameRate, 0, 'f', 3));

    if (auto* mk = m_project.markerAt(index)) {
        m_provenanceLabel->setText(
            tr("Marker: %1 — %2")
                .arg(markerPhaseName(mk->phase),
                     mk->state == MarkerState::Pending
                         ? tr("PENDING review (source replaced)")
                         : tr("active, bound to digest %1…")
                               .arg(m_project.source().sha256.left(10))));
    } else {
        m_provenanceLabel->clear();
    }
}

void MainWindow::refreshMarkerList()
{
    m_markerList->clear();
    for (const auto& m : m_project.markers()) {
        QString text = tr("#%1 %2  frame=%3  PTS=%4 µs")
                           .arg(m.id).arg(markerPhaseName(m.phase))
                           .arg(m.frameIndex).arg(static_cast<qint64>(m.ptsUs));
        if (m.state == MarkerState::Pending) text += tr("  [PENDING — 待核]");
        auto* item = new QListWidgetItem(text, m_markerList);
        if (m.state == MarkerState::Pending) item->setForeground(QColor("#b33"));
    }
    m_btnConfirm->setEnabled(false);
}

void MainWindow::refreshInfo() { showFrame(m_curFrame); }

void MainWindow::stepFirst() { if (m_decoder.isOpen()) showFrame(0); }
void MainWindow::stepPrev() { if (m_curFrame > 0) showFrame(m_curFrame - 1); }
void MainWindow::nextFrame() { if (m_curFrame + 1 < m_decoder.frameCount()) showFrame(m_curFrame + 1); }
void MainWindow::stepLast() { if (m_decoder.isOpen()) showFrame(m_decoder.frameCount() - 1); }
void MainWindow::onFrameSpin(int value) { showFrame(value); }

void MainWindow::markTakeoff()
{
    if (m_curFrame < 0) return;
    const FrameTime& ft = m_decoder.index()[m_curFrame];
    m_project.addMarker(m_curFrame, ft.ptsUs, MarkerPhase::Takeoff);
    refreshMarkerList();
    refreshInfo();
}
void MainWindow::markAir()
{
    if (m_curFrame < 0) return;
    const FrameTime& ft = m_decoder.index()[m_curFrame];
    m_project.addMarker(m_curFrame, ft.ptsUs, MarkerPhase::Air);
    refreshMarkerList();
    refreshInfo();
}
void MainWindow::markLanding()
{
    if (m_curFrame < 0) return;
    const FrameTime& ft = m_decoder.index()[m_curFrame];
    m_project.addMarker(m_curFrame, ft.ptsUs, MarkerPhase::Landing);
    refreshMarkerList();
    refreshInfo();
}

void MainWindow::removeSelectedMarker()
{
    const int row = m_markerList->currentRow();
    if (row < 0 || row >= m_project.markers().size()) return;
    m_project.removeMarker(m_project.markers()[row].id);
    refreshMarkerList();
    refreshInfo();
}

void MainWindow::onMarkerRowSelected(int row)
{
    if (row < 0 || row >= m_project.markers().size()) return;
    const auto& mk = m_project.markers()[row];
    if (mk.frameIndex >= 0 && mk.frameIndex < m_decoder.frameCount())
        showFrame(mk.frameIndex);
    m_btnConfirm->setEnabled(mk.state == MarkerState::Pending);
}

bool MainWindow::verifySourceNoDialog(QString* detail)
{
    QString local;
    const bool ok = m_project.verifySource(&local);
    if (detail) *detail = local;
    refreshMarkerList();
    refreshInfo();
    return ok;
}

void MainWindow::verifyMarkers()
{
    QString detail;
    const bool ok = verifySourceNoDialog(&detail);
    if (ok) {
        QMessageBox::information(this, tr("Source verification"),
                                 tr("Digest matches. All markers are active."));
    } else {
        QMessageBox::warning(this, tr("Source verification failed"),
                             tr("%1\n\nOld markers are now PENDING and must be "
                                "reviewed against the current material.")
                                 .arg(detail));
    }
}

void MainWindow::confirmPendingMarker()
{
    const int row = m_markerList->currentRow();
    if (row < 0 || row >= m_project.markers().size()) return;
    auto& mk = m_project.markers()[row];
    if (mk.frameIndex >= 0 && mk.frameIndex < m_decoder.frameCount())
        mk.ptsUs = m_decoder.index()[mk.frameIndex].ptsUs;
    mk.state = MarkerState::Active;
    refreshMarkerList();
    refreshInfo();
}

bool MainWindow::saveProjectTo(const QString& path)
{
    QString err;
    if (!m_project.save(path, &err)) {
        QMessageBox::critical(this, tr("Save failed"), err);
        return false;
    }
    return true;
}

void MainWindow::saveProject()
{
    QString path = m_project.path();
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, tr("Save project"),
                                            QStringLiteral("review.srtproj"),
                                            tr("Review projects (*.srtproj *.json)"));
        if (path.isEmpty()) return;
    }
    saveProjectTo(path);
}

void MainWindow::saveProjectAs()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save project as"), QStringLiteral("review.srtproj"),
        tr("Review projects (*.srtproj *.json)"));
    if (!path.isEmpty()) saveProjectTo(path);
}

bool MainWindow::loadProjectFrom(const QString& path)
{
    Project loaded;
    QString err;
    if (!loaded.load(path, &err)) {
        QMessageBox::critical(this, tr("Load failed"), err);
        return false;
    }
    QString detail;
    const bool verified = loaded.verifySource(&detail);

    if (!m_decoder.isOpen() || m_decoder.summary().sha256 != loaded.source().sha256) {
        if (!m_decoder.open(loaded.source().path, &err)
            || !m_decoder.buildIndex(nullptr, &err)) {
            QMessageBox::warning(this, tr("Source unavailable"),
                                 tr("Project loaded but its source cannot be opened:\n%1\n\n"
                                    "Markers remain saved and will be PENDING until the material "
                                    "is available again.").arg(err.isEmpty() ? detail : err));
            m_project = std::move(loaded);
            refreshMarkerList();
            return false;
        }
    }
    m_project = std::move(loaded);
    m_project.source() = m_decoder.summary();
    m_frameSpin->setRange(0, m_decoder.frameCount() - 1);
    setControlsEnabled(true);
    refreshMarkerList();
    showFrame(0);
    if (!verified)
        QMessageBox::warning(this, tr("Source changed"),
                             tr("%1\nMarkers are PENDING (待核). Confirm each one after review.")
                                 .arg(detail));
    return true;
}

void MainWindow::loadProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Load project"), QString(),
        tr("Review projects (*.srtproj *.json)"));
    if (!path.isEmpty()) loadProjectFrom(path);
}

void MainWindow::exportRange()
{
    if (!m_decoder.isOpen()) return;
    ExportDialog dlg(m_decoder.frameCount(),
                     m_decoder.summary().audioStreamIndex, this);
    if (dlg.exec() != QDialog::Accepted) return;
    ExportSettings s = dlg.settings();
    s.outputPath = QFileDialog::getSaveFileName(
        this, tr("Render output"), QStringLiteral("slowmo.mkv"),
        tr("Matroska (*.mkv);;MP4 (*.mp4)"));
    if (s.outputPath.isEmpty()) return;

    auto* prog = new QProgressDialog(tr("Rendering slow motion…"), QString(), 0, 100, this);
    prog->setWindowModality(Qt::WindowModal);
    prog->setMinimumDuration(0);
    Exporter exporter(m_decoder);
    ExportReport report;
    const bool ok = exporter.run(s, report, [&](int done, int total) {
        prog->setValue(total ? done * 100 / total : 0);
        QApplication::processEvents();
    });
    prog->close();
    if (!ok) {
        QMessageBox::critical(this, tr("Export failed"), report.error);
        return;
    }
    QMessageBox::information(
        this, tr("Export complete"),
        tr("Rendered: %1\nOutput frames: %2 (original %3, generated %4)\n"
           "Measured output duration: %5 µs; expected %6 µs\n"
           "Audio: %7\nProvenance sidecar: %8\n\n"
           "Generated frames are explicitly marked; verify independently with: srt_probe verify")
            .arg(report.outputPath).arg(report.outputFrameCount)
            .arg(report.originalFrameCount).arg(report.generatedFrameCount)
            .arg(static_cast<qint64>(report.outputDurationUs))
            .arg(static_cast<qint64>(report.expectedDurationUs))
            .arg(Exporter::audioModeName(report.audioMode))
            .arg(report.sidecarPath));
}

} // namespace sr
