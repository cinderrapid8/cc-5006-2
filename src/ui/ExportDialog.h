#pragma once

#include "core/Exporter.h"
#include "core/Project.h"
#include "core/SpeedCurve.h"
#include <QDialog>

class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;
class QTabWidget;

namespace sr {

class CurveEditor;

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    ExportDialog(int frameCount, int audioStreamIndex,
                 const QVector<FrameTime>& index,
                 const ExportConfig* initial,
                 QWidget* parent = nullptr);

    ExportSettings settings() const;
    ExportConfig exportConfig() const;
    int startFrame() const;
    int endFrame() const;
    bool acceptsCurve() const;

private slots:
    void updateSummary();

private:
    int m_frameCount;
    const QVector<FrameTime>& m_index;
    QSpinBox* m_start = nullptr;
    QSpinBox* m_end = nullptr;
    QDoubleSpinBox* m_speed = nullptr;
    QSpinBox* m_fps = nullptr;
    QComboBox* m_mode = nullptr;
    QComboBox* m_audio = nullptr;
    QComboBox* m_freezeAudio = nullptr;
    QCheckBox* m_useCurve = nullptr;
    QTabWidget* m_tabs = nullptr;
    CurveEditor* m_editor = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_audioNote = nullptr;
    int64_t currentSpanUs() const;
};

} // namespace sr
