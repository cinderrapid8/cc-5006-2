#include "ui/CurveEditor.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace sr {

// ---------------------------------------------------------------------------
// CurvePlot
// ---------------------------------------------------------------------------

CurvePlot::CurvePlot(CurveEditor* editor, QWidget* parent)
    : QWidget(parent), m_editor(editor)
{
    setMinimumHeight(300);
    setMouseTracking(true);
}

QSize CurvePlot::sizeHint() const { return {480, 320}; }

QRect CurvePlot::plotRect() const
{
    return rect().adjusted(kMarginLeft, kMarginTop, -kMarginRight, -kMarginBottom);
}

int CurvePlot::sourceUsToX(int64_t t) const
{
    const QRect r = plotRect();
    return r.left() + int(double(t) / double(m_spanUs) * r.width());
}

int64_t CurvePlot::xToSourceUs(int x) const
{
    const QRect r = plotRect();
    const double frac = double(qBound(r.left(), x, r.right()) - r.left()) / r.width();
    return static_cast<int64_t>(std::llround(frac * m_spanUs));
}

int64_t CurvePlot::safeOutputDurationUs() const
{
    SpeedCurve sc;
    sc.setKnots(m_knots);
    const int64_t d = sc.outputDurationUs();
    return d > 0 ? d : m_spanUs;
}

int CurvePlot::outputUsToY(int64_t t) const
{
    const QRect r = plotRect();
    return r.bottom() - int(double(t) / double(safeOutputDurationUs()) * r.height());
}

void CurvePlot::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QRect r = plotRect();
    p.fillRect(rect(), Qt::white);
    p.fillRect(r, QColor(245, 247, 250));

    SpeedCurve sc;
    sc.setKnots(m_knots);
    const int64_t outTotal = m_valid ? sc.outputDurationUs() : m_spanUs;

    // Grid + axis labels.
    p.setPen(QColor(210, 214, 220));
    for (int g = 0; g <= 4; ++g) {
        const int x = r.left() + r.width() * g / 4;
        const int y = r.bottom() - r.height() * g / 4;
        p.drawLine(x, r.top(), x, r.bottom());
        p.drawLine(r.left(), y, r.right(), y);
        p.setPen(QColor(90, 96, 104));
        p.drawText(x - 40, r.bottom() + 6, 80, 24, Qt::AlignHCenter,
                   QStringLiteral("%1 ms").arg(m_spanUs * g / 4 / 1000));
        p.drawText(4, y - 9, kMarginLeft - 8, 18, Qt::AlignRight,
                   QStringLiteral("%1 ms").arg(outTotal * g / 4 / 1000));
        p.setPen(QColor(210, 214, 220));
    }

    // Mapping polyline evaluated at every horizontal pixel, so the drawn
    // curve is the actual mapping used by the planner.
    if (m_valid) {
        p.setPen(QPen(QColor(32, 96, 200), 2));
        QPoint prev;
        bool first = true;
        for (int x = r.left(); x <= r.right(); ++x) {
            const int64_t u = xToSourceUs(x);
            const int64_t s = sc.sourceToOutput(qBound<int64_t>(0, u, m_spanUs));
            const QPoint pt(x, outputUsToY(qMin(s, outTotal)));
            if (!first) p.drawLine(prev, pt);
            prev = pt;
            first = false;
        }
    } else {
        p.setPen(QColor(180, 60, 60));
        p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap,
                   tr("Invalid curve: %1").arg(m_reason));
    }

    // Knot handles; freeze knots drawn as diamonds.
    for (int i = 0; i < m_knots.size(); ++i) {
        const auto& k = m_knots[i];
        const int x = sourceUsToX(k.timeUs);
        const int y = outputUsToY(qMin(sc.sourceToOutput(k.timeUs), outTotal));
        const bool selected = i == m_dragKnot;
        p.setBrush(selected ? QColor(220, 90, 40) : QColor(32, 96, 200));
        p.setPen(Qt::black);
        if (k.freezeDurationUs > 0) {
            const QPolygon diamond{{x, y - kHandleRadius - 2},
                                   {x + kHandleRadius + 2, y},
                                   {x, y + kHandleRadius + 2},
                                   {x - kHandleRadius - 2, y}};
            p.drawPolygon(diamond);
        } else {
            p.drawEllipse(QPoint(x, y), kHandleRadius, kHandleRadius);
        }
    }

    p.setPen(QColor(40, 44, 50));
    p.drawText(r.left(), r.top() - 2,
               tr("Output time vs source time — drag control points horizontally; "
                  "diamonds mark freeze holds"));
}

int CurvePlot::knotAt(const QPoint& pos) const
{
    SpeedCurve sc;
    sc.setKnots(m_knots);
    const int64_t outTotal = safeOutputDurationUs();
    for (int i = 0; i < m_knots.size(); ++i) {
        const int x = sourceUsToX(m_knots[i].timeUs);
        const int y = outputUsToY(qMin(sc.sourceToOutput(m_knots[i].timeUs), outTotal));
        if (QLineF(pos, QPoint(x, y)).length() <= kHandleRadius + 4)
            return i;
    }
    return -1;
}

void CurvePlot::mousePressEvent(QMouseEvent* e)
{
    const int hit = knotAt(e->pos());
    if (hit >= 0) m_dragKnot = hit;
}

void CurvePlot::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragKnot < 0) return;
    if (m_dragKnot == 0 || m_dragKnot == m_knots.size() - 1)
        return; // anchors stay fixed at the interval ends
    int64_t t = xToSourceUs(e->pos().x());
    const int64_t lo = m_knots[m_dragKnot - 1].timeUs + SpeedCurve::minTimeGapUs;
    const int64_t hi = m_knots[m_dragKnot + 1].timeUs - SpeedCurve::minTimeGapUs;
    t = qBound(lo, t, hi);
    if (t != m_knots[m_dragKnot].timeUs) {
        m_knots[m_dragKnot].timeUs = t;
        update();
        emit dragFinished();
    }
}

void CurvePlot::mouseReleaseEvent(QMouseEvent*)
{
    if (m_dragKnot >= 0) {
        m_dragKnot = -1;
        update();
    }
}

// ---------------------------------------------------------------------------
// CurveEditor
// ---------------------------------------------------------------------------

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(360);
    auto* root = new QVBoxLayout(this);

    auto* plotRow = new QHBoxLayout;
    m_plot = new CurvePlot(this, this);
    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels(
        {tr("Source time (µs)"), tr("Speed after point"), tr("Freeze hold (µs)")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setMinimumWidth(380);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    plotRow->addWidget(m_plot, 3);
    plotRow->addWidget(m_table, 2);
    root->addLayout(plotRow, 1);

    auto* btnRow = new QHBoxLayout;
    m_addBtn = new QPushButton(tr("Add control point"));
    m_removeBtn = new QPushButton(tr("Remove selected point"));
    m_resetBtn = new QPushButton(tr("Reset to constant slow motion"));
    btnRow->addWidget(m_addBtn);
    btnRow->addWidget(m_removeBtn);
    btnRow->addWidget(m_resetBtn);
    btnRow->addStretch();
    root->addLayout(btnRow);

    m_metrics = new QLabel;
    m_metrics->setWordWrap(true);
    m_metrics->setStyleSheet("font-weight:bold;");
    root->addWidget(m_metrics);

    connect(m_table, &QTableWidget::cellChanged, this, &CurveEditor::onCellChanged);
    connect(m_table, &QTableWidget::currentCellChanged,
            this, [this](int r, int, int, int) { m_selectedRow = r; });
    connect(m_addBtn, &QPushButton::clicked, this, &CurveEditor::addKnot);
    connect(m_removeBtn, &QPushButton::clicked, this, &CurveEditor::removeSelectedKnot);
    connect(m_resetBtn, &QPushButton::clicked, this, &CurveEditor::resetConstantClicked);
    connect(m_plot, &CurvePlot::dragFinished, this, &CurveEditor::onPlotDrag);

    m_knots = {{0, 0.5, 0}, {m_spanUs, 0.5, 0}};
    rebuildTable();
    refreshMetrics();
}

void CurveEditor::onPlotDrag()
{
    // The plot owns the dragged knot times; pull them back and rebuild the
    // table without re-entering the cell editor.
    m_knots = m_plot->knots();
    rebuildTable();
    refreshMetrics();
    emit changed();
}

void CurveEditor::setSourceSpan(int64_t spanUs, bool clampKnots)
{
    if (spanUs <= 0) return;
    m_spanUs = spanUs;
    if (clampKnots) {
        for (auto& k : m_knots)
            k.timeUs = qBound<int64_t>(0, k.timeUs, spanUs);
        m_knots.first().timeUs = 0;
        m_knots.last().timeUs = spanUs;
    }
    afterKnotsChanged();
}

void CurveEditor::setCurve(const SpeedCurve& curve)
{
    m_knots = curve.knots();
    if (m_knots.size() < 2)
        m_knots = {{0, 0.5, 0}, {m_spanUs, 0.5, 0}};
    // A curve loaded from a saved export config already targets the current
    // span; preserve its knot times verbatim rather than re-anchoring.
    afterKnotsChanged();
}

SpeedCurve CurveEditor::curve() const
{
    SpeedCurve c;
    c.setKnots(m_knots);
    return c;
}

bool CurveEditor::isValid(QString* reason) const
{
    return curve().validate(m_spanUs, reason);
}

int64_t CurveEditor::outputDurationUs() const
{
    QString r;
    if (!isValid(&r)) return -1;
    return curve().outputDurationUs();
}

void CurveEditor::resetToConstant(double speed)
{
    m_knots = {{0, speed, 0}, {m_spanUs, speed, 0}};
    afterKnotsChanged();
    emit changed();
}

void CurveEditor::afterKnotsChanged()
{
    rebuildTable();
    m_plot->setKnots(m_knots);
    m_plot->setSourceSpan(m_spanUs);
    QString reason;
    const bool valid = isValid(&reason);
    m_plot->setValid(valid, reason);
    refreshMetrics();
    update();
}

void CurveEditor::rebuildTable()
{
    m_syncing = true;
    m_table->setRowCount(m_knots.size());
    for (int i = 0; i < m_knots.size(); ++i) {
        const auto& k = m_knots[i];
        auto* tItem = new QTableWidgetItem(QString::number(k.timeUs));
        auto* sItem = new QTableWidgetItem(QString::number(k.speed, 'f', 4));
        auto* fItem = new QTableWidgetItem(QString::number(k.freezeDurationUs));
        if (i == 0 || i == m_knots.size() - 1) {
            tItem->setFlags(tItem->flags() & ~Qt::ItemIsEditable);
            tItem->setBackground(QColor(235, 238, 242));
        }
        m_table->setItem(i, 0, tItem);
        m_table->setItem(i, 1, sItem);
        m_table->setItem(i, 2, fItem);
    }
    m_syncing = false;
}

void CurveEditor::onCellChanged(int row, int)
{
    if (m_syncing) return;
    QVector<SpeedKnot> next = m_knots;
    for (int i = 0; i < m_table->rowCount() && i < next.size(); ++i) {
        next[i].timeUs = m_table->item(i, 0)->text().toLongLong();
        next[i].speed = m_table->item(i, 1)->text().toDouble();
        next[i].freezeDurationUs = m_table->item(i, 2)->text().toLongLong();
    }
    next.first().timeUs = 0;
    next.last().timeUs = m_spanUs;
    std::sort(next.begin(), next.end(),
              [](const SpeedKnot& a, const SpeedKnot& b) { return a.timeUs < b.timeUs; });
    m_knots = std::move(next);
    m_selectedRow = row;
    afterKnotsChanged();
    emit changed();
}

void CurveEditor::addKnot()
{
    if (m_knots.size() < 2) return;
    int best = 0;
    int64_t bestGap = -1;
    for (int i = 0; i + 1 < m_knots.size(); ++i) {
        const int64_t gap = m_knots[i + 1].timeUs - m_knots[i].timeUs;
        if (gap > bestGap) { bestGap = gap; best = i; }
    }
    if (bestGap < 2 * SpeedCurve::minTimeGapUs + 1) return;
    SpeedKnot k;
    k.timeUs = m_knots[best].timeUs + bestGap / 2;
    k.speed = m_knots[best].speed;
    k.freezeDurationUs = 0;
    m_knots.insert(best + 1, k);
    m_selectedRow = best + 1;
    afterKnotsChanged();
    m_table->selectRow(best + 1);
    emit changed();
}

void CurveEditor::removeSelectedKnot()
{
    const int row = m_table->currentRow();
    if (row <= 0 || row >= m_knots.size() - 1 || row < 0) return;
    m_knots.removeAt(row);
    m_selectedRow = -1;
    afterKnotsChanged();
    emit changed();
}

void CurveEditor::resetConstantClicked()
{
    resetToConstant(m_knots.isEmpty() ? 0.5 : m_knots.first().speed);
}

void CurveEditor::refreshMetrics()
{
    SpeedCurve sc;
    sc.setKnots(m_knots);
    QString reason;
    if (!sc.validate(m_spanUs, &reason)) {
        m_metrics->setStyleSheet("color:#b33;font-weight:bold;");
        m_metrics->setText(tr("Invalid curve: %1").arg(reason));
        return;
    }
    const int64_t out = sc.outputDurationUs();
    const double factor = double(out) / double(m_spanUs);
    m_metrics->setStyleSheet("font-weight:bold;color:#0a5;");
    m_metrics->setText(
        tr("Source span: %1 µs   |   Actual output length: %2 µs (%3 s, %4x source)   "
           "|   control points: %5")
            .arg(m_spanUs)
            .arg(out)
            .arg(out / 1e6, 0, 'f', 6)
            .arg(factor, 0, 'f', 3)
            .arg(m_knots.size()));
}

} // namespace sr
