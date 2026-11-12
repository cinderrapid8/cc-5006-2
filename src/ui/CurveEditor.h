#pragma once

#include "core/SpeedCurve.h"
#include <QWidget>

class QTableWidget;
class QLabel;
class QPushButton;

namespace sr {

class CurvePlot;

// Interactive editor for the segmented source->output speed mapping.
//
// The widget owns an ordered list of knots and renders them on a separate
// CurvePlot canvas (output time on the vertical axis, source time on the
// horizontal). Knots can be dragged along the source-time axis; speed and
// freeze length are edited in the adjacent table. Every edit re-validates the
// curve and emits changed() with the concrete output length, so the caller
// always displays the real rendered duration rather than an estimate.
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);

    // Span of the exported source interval in microseconds. Knot times are
    // span-relative; the first knot is always anchored at 0 and the last at
    // spanUs. When clampKnots is false only the span changes (used while
    // loading a saved curve whose knots already target this span).
    void setSourceSpan(int64_t spanUs, bool clampKnots = true);
    int64_t sourceSpan() const { return m_spanUs; }

    // Replace the whole knot set (e.g. when switching intervals or loading a
    // saved project). Times outside [0, span] are clamped.
    void setCurve(const SpeedCurve& curve);
    SpeedCurve curve() const;

    // A valid curve ready for planning, plus the validation message if not.
    bool isValid(QString* reason = nullptr) const;
    int64_t outputDurationUs() const;

    // Reset to the historical constant slow-motion special case.
    void resetToConstant(double speed);

    const QVector<SpeedKnot>& knots() const { return m_knots; }
    void setKnotsForTest(QVector<SpeedKnot> k) { m_knots = std::move(k); afterKnotsChanged(); }

signals:
    void changed();

private slots:
    void onCellChanged(int row, int column);
    void addKnot();
    void removeSelectedKnot();
    void resetConstantClicked();
    void onPlotDrag();

private:
    void rebuildTable();
    void afterKnotsChanged();
    void refreshMetrics();

    int64_t m_spanUs = 1'000'000;
    QVector<SpeedKnot> m_knots;
    int m_selectedRow = -1;

    CurvePlot* m_plot = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_metrics = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_removeBtn = nullptr;
    QPushButton* m_resetBtn = nullptr;
    bool m_syncing = false;
};

// The drawing surface. Kept as a sibling child of the table inside the
// editor's layout (a widget must never insert itself into its own layout).
class CurvePlot : public QWidget {
    Q_OBJECT
public:
    explicit CurvePlot(CurveEditor* editor, QWidget* parent = nullptr);

    void setKnots(const QVector<SpeedKnot>& knots) { m_knots = knots; update(); }
    const QVector<SpeedKnot>& knots() const { return m_knots; }
    void setSourceSpan(int64_t spanUs) { m_spanUs = spanUs; update(); }
    void setValid(bool valid, const QString& reason) { m_valid = valid; m_reason = reason; update(); }

signals:
    void dragFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;

private:
    int knotAt(const QPoint& pos) const;
    QRect plotRect() const;
    int64_t xToSourceUs(int x) const;
    int sourceUsToX(int64_t t) const;
    int outputUsToY(int64_t t) const;
    int64_t safeOutputDurationUs() const;

    CurveEditor* m_editor;
    QVector<SpeedKnot> m_knots;
    int64_t m_spanUs = 1'000'000;
    int m_dragKnot = -1;
    bool m_valid = true;
    QString m_reason;

    static constexpr int kMarginLeft = 64;
    static constexpr int kMarginRight = 20;
    static constexpr int kMarginTop = 16;
    static constexpr int kMarginBottom = 40;
    static constexpr int kHandleRadius = 6;
};

} // namespace sr
