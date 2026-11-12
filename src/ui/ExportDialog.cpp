#include "ui/ExportDialog.h"

#include "ui/CurveEditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace sr {

ExportDialog::ExportDialog(int frameCount, int audioStreamIndex,
                           const QVector<FrameTime>& index,
                           const ExportConfig* initial,
                           QWidget* parent)
    : QDialog(parent), m_frameCount(frameCount), m_index(index)
{
    setWindowTitle(tr("Export slow-motion interval"));
    setMinimumSize(960, 720);
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

    // Freeze policy only applies to segmented curves with explicit holds.
    m_freezeAudio = new QComboBox;
    m_freezeAudio->addItem(tr("Silence during freeze holds"),
                           static_cast<int>(AudioFreezePolicy::Silence));
    m_freezeAudio->addItem(tr("Hold last source sample during holds"),
                           static_cast<int>(AudioFreezePolicy::HoldLast));
    m_freezeAudio->setEnabled(audioStreamIndex >= 0);

    m_useCurve = new QCheckBox(
        tr("Use segmented speed curve (entry → slowdown → freeze → acceleration)"));

    form->addRow(tr("Start frame (inclusive):"), m_start);
    form->addRow(tr("End frame (inclusive):"), m_end);
    form->addRow(tr("Playback speed (constant special case):"), m_speed);
    form->addRow(tr("Output frame rate:"), m_fps);
    form->addRow(tr("Slow-motion strategy:"), m_mode);
    form->addRow(tr("Audio:"), m_audio);
    form->addRow(tr("Audio on freeze segments:"), m_freezeAudio);
    form->addRow(m_useCurve);

    m_audioNote = new QLabel;
    m_audioNote->setWordWrap(true);
    m_audioNote->setStyleSheet("color:#666;");

    m_summary = new QLabel;
    m_summary->setWordWrap(true);

    m_editor = new CurveEditor;
    m_editor->setEnabled(false);

    m_tabs = new QTabWidget;
    auto* settingsPage = new QWidget;
    auto* settingsLay = new QVBoxLayout(settingsPage);
    settingsLay->addLayout(form);
    settingsLay->addWidget(m_audioNote);
    settingsLay->addStretch();
    m_tabs->addTab(settingsPage, tr("Interval & audio"));
    m_tabs->addTab(m_editor, tr("Segmented speed curve"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_useCurve->isChecked()) {
            QString reason;
            if (!m_editor->isValid(&reason)) {
                m_summary->setStyleSheet("color:#b33;font-weight:bold;");
                m_summary->setText(tr("Cannot export: %1").arg(reason));
                m_tabs->setCurrentWidget(m_editor);
                return;
            }
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addWidget(m_tabs);
    root->addWidget(m_summary);
    root->addWidget(buttons);

    connect(m_start, &QSpinBox::valueChanged, this, [this](int){ updateSummary(); });
    connect(m_end, &QSpinBox::valueChanged, this, [this](int){ updateSummary(); });
    connect(m_speed, &QDoubleSpinBox::valueChanged, this, [this](double){ updateSummary(); });
    connect(m_fps, &QSpinBox::valueChanged, this, [this](int){ updateSummary(); });
    connect(m_audio, &QComboBox::currentIndexChanged, this, [this](int){
        const auto mode = static_cast<AudioMode>(m_audio->currentData().toInt());
        if (mode == AudioMode::Drop)
            m_audioNote->setText(tr("Note: resampling stretches the waveform; pitch drops "
                                    "with speed. Pitch is not preserved in this mode."));
        else if (mode == AudioMode::Atempo)
            m_audioNote->setText(tr("Note: atempo changes tempo while preserving pitch via "
                                    "libavfilter. Each curve segment gets its own fully "
                                    "flushed filter graph, so algorithmic delay cannot cross "
                                    "a speed change."));
        else
            m_audioNote->setText(tr("Muted: the rendered file contains no audio stream."));
    });
    connect(m_useCurve, &QCheckBox::toggled, this, [this](bool on) {
        m_editor->setEnabled(on);
        m_speed->setEnabled(!on);
        m_tabs->setCurrentWidget(on ? static_cast<QWidget*>(m_editor)
                                    : static_cast<QWidget*>(m_tabs->widget(0)));
        updateSummary();
    });
    connect(m_editor, &CurveEditor::changed, this, [this]{ updateSummary(); });

    if (initial) {
        m_start->setValue(qBound(0, initial->startFrame, frameCount - 1));
        m_end->setValue(qBound(m_start->value(), initial->endFrame, frameCount - 1));
        m_speed->setValue(initial->speed);
        m_fps->setValue(initial->outputFrameRate);
        m_mode->setCurrentIndex(m_mode->findData(initial->slowmoMode));
        m_audio->setCurrentIndex(m_audio->findData(initial->audioMode));
        m_freezeAudio->setCurrentIndex(
            m_freezeAudio->findData(initial->audioFreezePolicy));
        if (initial->hasCurve && initial->curve.knots().size() >= 2) {
            // Loaded curves already target the interval's span; set the span
            // without clamping, then restore the knot set verbatim.
            m_editor->setSourceSpan(currentSpanUs(), false);
            m_editor->setCurve(initial->curve);
            m_useCurve->setChecked(true);
        }
    }
    updateSummary();
}

int64_t ExportDialog::currentSpanUs() const
{
    const int a = m_start->value();
    const int b = qMax(a, m_end->value());
    if (m_index.isEmpty() || b >= m_index.size()) return 1'000'000;
    return m_index[b].ptsUs + m_index[b].durationUs - m_index[a].ptsUs;
}

int ExportDialog::startFrame() const { return m_start->value(); }
int ExportDialog::endFrame() const { return qMax(m_start->value(), m_end->value()); }
bool ExportDialog::acceptsCurve() const { return m_useCurve->isChecked(); }

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
    s.audioFreezePolicy = m_freezeAudio->isEnabled()
                              ? static_cast<AudioFreezePolicy>(
                                    m_freezeAudio->currentData().toInt())
                              : AudioFreezePolicy::Silence;
    s.hasCurve = m_useCurve->isChecked();
    if (s.hasCurve) s.curve = m_editor->curve();
    return s;
}

ExportConfig ExportDialog::exportConfig() const
{
    const auto s = settings();
    ExportConfig c;
    c.startFrame = s.startFrame;
    c.endFrame = s.endFrame;
    c.hasCurve = s.hasCurve;
    c.curve = s.curve;
    c.speed = s.speed;
    c.slowmoMode = static_cast<int>(s.slowmoMode);
    c.audioMode = static_cast<int>(s.audioMode);
    c.audioFreezePolicy = static_cast<int>(s.audioFreezePolicy);
    c.outputFrameRate = s.outputFrameRate;
    return c;
}

void ExportDialog::updateSummary()
{
    if (m_end->value() < m_start->value()) {
        const QSignalBlocker blocker(m_end);
        m_end->setValue(m_start->value());
    }

    // Only re-anchor the editor when the interval actually changes; otherwise
    // a loaded curve would be silently re-clamped to the freshly computed span.
    const int64_t span = currentSpanUs();
    if (m_editor->sourceSpan() != span)
        m_editor->setSourceSpan(span);

    const int frames = m_end->value() - m_start->value() + 1;
    if (!m_useCurve->isChecked()) {
        m_summary->setStyleSheet("");
        m_summary->setText(
            tr("Constant slow motion (curve special case): %1 source frames (%2..%3). "
               "At speed %4, output duration will be %5x the source span; every generated "
               "frame is labelled original/repeat/interpolated/freeze in the .frames.json "
               "sidecar.")
                .arg(frames).arg(m_start->value()).arg(m_end->value())
                .arg(m_speed->value(), 0, 'f', 2)
                .arg(1.0 / m_speed->value(), 0, 'f', 2));
        return;
    }
    QString reason;
    if (!m_editor->isValid(&reason)) {
        m_summary->setStyleSheet("color:#b33;font-weight:bold;");
        m_summary->setText(tr("Invalid curve: %1").arg(reason));
        return;
    }
    // This readout is computed from the same SpeedCurve the exporter plans.
    const int64_t out = m_editor->outputDurationUs();
    m_summary->setStyleSheet("font-weight:bold;color:#0a5;");
    m_summary->setText(
        tr("Segmented curve over %1 source frames (%2..%3). Actual output length: "
           "%4 µs (%5 s) at %6 fps. Preview frame selection and the rendered file use "
           "this identical mapping.")
            .arg(frames).arg(m_start->value()).arg(m_end->value())
            .arg(out).arg(out / 1e6, 0, 'f', 6).arg(m_fps->value()));
}

} // namespace sr
