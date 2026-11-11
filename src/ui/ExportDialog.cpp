#include "ui/ExportDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

namespace sr {

ExportDialog::ExportDialog(int frameCount, int audioStreamIndex, QWidget* parent)
    : QDialog(parent), m_frameCount(frameCount)
{
    setWindowTitle(tr("Export slow-motion interval"));
    auto* form = new QFormLayout;

    m_start = new QSpinBox;
    m_start->setRange(0, frameCount - 1);
    m_end = new QSpinBox;
    m_end->setRange(0, frameCount - 1);
    m_end->setValue(frameCount - 1);

    m_speed = new QDoubleSpinBox;
    m_speed->setRange(0.05, 1.0);
    m_speed->setSingleStep(0.05);
    m_speed->setValue(0.5);
    m_speed->setDecimals(2);

    m_fps = new QSpinBox;
    m_fps->setRange(1, 120);
    m_fps->setValue(60);
    m_fps->setSuffix(" fps");

    m_mode = new QComboBox;
    m_mode->addItem(tr("Repeat generated frames (no synthesis)"),
                    static_cast<int>(SlowmoMode::Repeat));
    m_mode->addItem(tr("Linear interpolation between source frames (generated)"),
                    static_cast<int>(SlowmoMode::Interpolate));

    m_audio = new QComboBox;
    m_audio->addItem(tr("Mute — output has no audio track"),
                     static_cast<int>(AudioMode::Mute));
    m_audio->addItem(tr("Stretch by resampling — pitch shifts, NOT preserved"),
                     static_cast<int>(AudioMode::Drop));
    m_audio->setEnabled(audioStreamIndex >= 0);
    if (audioStreamIndex < 0)
        m_audio->addItem(tr("Source has no audio track"), static_cast<int>(AudioMode::Mute));
    else
        m_audio->addItem(tr("atempo filter — tempo changed, pitch preserved"),
                         static_cast<int>(AudioMode::Atempo));

    form->addRow(tr("Start frame (inclusive):"), m_start);
    form->addRow(tr("End frame (inclusive):"), m_end);
    form->addRow(tr("Playback speed:"), m_speed);
    form->addRow(tr("Output frame rate:"), m_fps);
    form->addRow(tr("Slow-motion strategy:"), m_mode);
    form->addRow(tr("Audio:"), m_audio);

    m_audioNote = new QLabel;
    m_audioNote->setWordWrap(true);
    m_audioNote->setStyleSheet("color:#666;");

    m_summary = new QLabel;
    m_summary->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(m_audioNote);
    root->addWidget(m_summary);
    root->addWidget(buttons);

    connect(m_start, &QSpinBox::valueChanged, this, [this](int){ updateSummary(); });
    connect(m_end, &QSpinBox::valueChanged, this, [this](int){ updateSummary(); });
    connect(m_speed, &QDoubleSpinBox::valueChanged, this, [this](double){ updateSummary(); });
    connect(m_fps, &QSpinBox::valueChanged, this, [this](int){ updateSummary(); });
    connect(m_audio, &QComboBox::currentIndexChanged, this, [this](int){
        const auto mode = static_cast<AudioMode>(m_audio->currentData().toInt());
        if (mode == AudioMode::Drop)
            m_audioNote->setText(tr("Note: resampling stretches the waveform; pitch drops with speed. "
                                    "Pitch is not preserved in this mode."));
        else if (mode == AudioMode::Atempo)
            m_audioNote->setText(tr("Note: atempo changes tempo while preserving pitch (libavfilter)."));
        else
            m_audioNote->setText(tr("Muted: the rendered file contains no audio stream."));
    });
    updateSummary();
}

int ExportDialog::startFrame() const { return m_start->value(); }
int ExportDialog::endFrame() const { return m_end->value(); }

ExportSettings ExportDialog::settings() const
{
    ExportSettings s;
    s.startFrame = m_start->value();
    s.endFrame = qMax(m_start->value(), m_end->value());
    s.speed = m_speed->value();
    s.outputFrameRate = m_fps->value();
    s.slowmoMode = static_cast<SlowmoMode>(m_mode->currentData().toInt());
    s.audioMode = m_audio->isEnabled()
        ? static_cast<AudioMode>(m_audio->currentData().toInt())
        : AudioMode::Mute;
    return s;
}

void ExportDialog::updateSummary()
{
    if (m_end->value() < m_start->value()) m_end->setValue(m_start->value());
    const int frames = m_end->value() - m_start->value() + 1;
    m_summary->setText(tr("Interval: %1 source frames (%2..%3). At speed %4, output duration "
                          "will be %5x the source span; every generated frame is labelled "
                          "original/repeat/interpolated in the .frames.json sidecar.")
                           .arg(frames).arg(m_start->value()).arg(m_end->value())
                           .arg(m_speed->value(), 0, 'f', 2)
                           .arg(1.0 / m_speed->value(), 0, 'f', 2));
}

} // namespace sr
