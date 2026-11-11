#pragma once

#include "core/Exporter.h"
#include <QDialog>

class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;

namespace sr {

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    ExportDialog(int frameCount, int audioStreamIndex, QWidget* parent = nullptr);

    ExportSettings settings() const;
    int startFrame() const;
    int endFrame() const;

private slots:
    void updateSummary();

private:
    int m_frameCount;
    QSpinBox* m_start = nullptr;
    QSpinBox* m_end = nullptr;
    QDoubleSpinBox* m_speed = nullptr;
    QSpinBox* m_fps = nullptr;
    QComboBox* m_mode = nullptr;
    QComboBox* m_audio = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_audioNote = nullptr;
};

} // namespace sr
