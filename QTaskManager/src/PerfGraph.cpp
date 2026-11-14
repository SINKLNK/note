#include "PerfGraph.h"

#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>

namespace
{
const QColor kGridColor(224, 230, 236);
}

PerfGraph::PerfGraph(const QColor &color, QWidget *parent)
    : QWidget(parent)
    , m_color(color)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void PerfGraph::setGrid(int columns, int rows)
{
    m_columns = qMax(1, columns);
    m_rows = qMax(1, rows);
    update();
}

void PerfGraph::setCapacity(int samples)
{
    m_capacity = qMax(2, samples);
    trim(m_values);
    trim(m_secondary);
    update();
}

void PerfGraph::setMaximum(double maximum)
{
    m_maximum = maximum > 0.0 ? maximum : 1.0;
    update();
}

void PerfGraph::setHoverUnit(const QString &unit)
{
    m_hoverUnit = unit;
    m_hoverX = -1.0;
    setMouseTracking(!unit.isEmpty());
    update();
}

void PerfGraph::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_hoverUnit.isEmpty()) {
        m_hoverX = event->position().x();
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void PerfGraph::leaveEvent(QEvent *event)
{
    m_hoverX = -1.0;
    update();
    QWidget::leaveEvent(event);
}

void PerfGraph::hideEvent(QHideEvent *event)
{
    m_hoverX = -1.0;
    QWidget::hideEvent(event);
}

void PerfGraph::appendValue(double value)
{
    m_values.append(value);
    trim(m_values);
    update();
}

void PerfGraph::appendSecondaryValue(double value)
{
    m_secondary.append(value);
    trim(m_secondary);
    update();
}

void PerfGraph::trim(QVector<double> &values) const
{
    if (values.size() > m_capacity) {
        values.remove(0, values.size() - m_capacity);
    }
}

void PerfGraph::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);

    painter.setPen(QPen(kGridColor, 1));
    for (int column = 1; column < m_columns; ++column) {
        const int x = width() * column / m_columns;
        painter.drawLine(x, 0, x, height());
    }
    for (int row = 1; row < m_rows; ++row) {
        const int y = height() * row / m_rows;
        painter.drawLine(0, y, width(), y);
    }

    painter.setRenderHint(QPainter::Antialiasing);
    drawSeries(painter, m_values, 60);
    drawSeries(painter, m_secondary, 55);
    painter.setRenderHint(QPainter::Antialiasing, false);

    QColor border = m_color;
    border.setAlpha(150);
    painter.setPen(border);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));

    drawHover(painter);
}

QPointF PerfGraph::samplePoint(int index, const QVector<double> &values) const
{
    const qreal right = width() - 1;
    const qreal bottom = height() - 1;
    const qreal step = right / (m_capacity - 1);
    const qreal ratio = qBound(0.0, values[index] / m_maximum, 1.0);
    return QPointF(right - (values.size() - 1 - index) * step, bottom - ratio * bottom);
}

void PerfGraph::drawSeries(QPainter &painter, const QVector<double> &values, int fillAlpha) const
{
    const int count = static_cast<int>(values.size());
    if (count < 2) {
        return;
    }

    // Samples sit at fixed slots counted back from the right edge, so a
    // half-filled history occupies the right part of the chart, like in Task
    // Manager right after start-up.
    const qreal bottom = height() - 1;

    QPolygonF line;
    line.reserve(count);
    for (int i = 0; i < count; ++i) {
        line << samplePoint(i, values);
    }

    QPolygonF area = line;
    area << QPointF(line.last().x(), bottom) << QPointF(line.first().x(), bottom);

    QColor fill = m_color;
    fill.setAlpha(fillAlpha);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawPolygon(area);

    painter.setPen(QPen(m_color, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(line);
}

void PerfGraph::drawHover(QPainter &painter) const
{
    if (m_hoverUnit.isEmpty() || m_hoverX < 0.0 || m_values.isEmpty()
        || width() <= 1 || height() <= 1) {
        return;
    }

    const qreal right = width() - 1;
    // Do not select a sample in the empty part of a partially filled history.
    if (m_hoverX < samplePoint(0, m_values).x() || m_hoverX > right) {
        return;
    }
    const qreal step = right / (m_capacity - 1);
    const int index = qBound(0, static_cast<int>(m_values.size()) - 1
                                   - qRound((right - m_hoverX) / step),
                             static_cast<int>(m_values.size()) - 1);
    const QPointF point = samplePoint(index, m_values);

    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::white, 2));
    painter.setBrush(m_color);
    painter.drawEllipse(point, 4.5, 4.5);

    const QString text = locale().toString(m_values[index], 'f', 1)
                         + QLatin1Char(' ') + m_hoverUnit;
    const QFontMetrics metrics(painter.font());
    const QRectF bounds = QRectF(rect()).adjusted(4, 4, -4, -4);
    if (bounds.width() < 16 || bounds.height() < metrics.height() + 6) {
        return;
    }
    const QSizeF size(qMin(bounds.width(), qreal(metrics.horizontalAdvance(text) + 12)),
                      metrics.height() + 6);
    qreal x = point.x() + 10;
    if (x + size.width() > bounds.right()) {
        x = point.x() - 10 - size.width();
    }
    qreal y = point.y() - 10 - size.height();
    if (y < bounds.top()) {
        y = point.y() + 10;
    }
    x = qBound(bounds.left(), x, bounds.right() - size.width());
    y = qBound(bounds.top(), y, bounds.bottom() - size.height());
    const QRectF label(QPointF(x, y), size);

    painter.setPen(QPen(m_color, 1));
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(label, 4, 4);
    painter.setPen(QColor(32, 40, 56));
    painter.drawText(label.adjusted(6, 3, -6, -3), Qt::AlignCenter,
                     metrics.elidedText(text, Qt::ElideRight, int(size.width()) - 12));
}
