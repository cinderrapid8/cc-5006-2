#pragma once

#include "core/Project.h"
#include "core/VideoDecoder.h"
#include <QLabel>
#include <QMainWindow>
#include <QListWidget>
#include <QSpinBox>

class QPushButton;
class QCheckBox;

namespace sr {

class ExportDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void openSource();
    void saveProject();
    void saveProjectAs();
    void loadProject();
    void markTakeoff();
    void markAir();
    void markLanding();
    void removeSelectedMarker();
    void verifyMarkers();
    void confirmPendingMarker();
    void exportRange();
    void stepFirst();
    void stepPrev();
    void nextFrame();
    void stepLast();
    void onMarkerRowSelected(int row);
    void onFrameSpin(int value);

public:
    // Core operations, callable without a modal file dialog (used by tests).
    bool openSourceFile(const QString& path);
    bool saveProjectTo(const QString& path);
    bool loadProjectFrom(const QString& path);
    // Model-level verification without popping a modal message box.
    bool verifySourceNoDialog(QString* detail = nullptr);
    int markerCount() const;
    int currentFrame() const { return m_curFrame; }

private:
    void buildUi();
    void showFrame(int index);
    void refreshMarkerList();
    void refreshInfo();
    void setControlsEnabled(bool on);
    MarkerPhase currentPhaseButton() const;
    void closeSource();

    VideoDecoder m_decoder;
    Project m_project;
    int m_curFrame = -1;

    QLabel* m_videoLabel = nullptr;
    QLabel* m_infoLabel = nullptr;
    QLabel* m_provenanceLabel = nullptr;
    QLabel* m_sourceStatusLabel = nullptr;
    QSpinBox* m_frameSpin = nullptr;
    QListWidget* m_markerList = nullptr;
    QPushButton* m_btnTakeoff = nullptr;
    QPushButton* m_btnAir = nullptr;
    QPushButton* m_btnLanding = nullptr;
    QPushButton* m_btnVerify = nullptr;
    QPushButton* m_btnConfirm = nullptr;
    QPushButton* m_btnExport = nullptr;
    ExportDialog* m_exportDialog = nullptr;
};

} // namespace sr
