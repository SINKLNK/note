#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

class QPainter;

// A Task Manager style history chart: a grid, the newest sample on the right
// edge, each series drawn as a line over a translucent fill. An optional
// secondary series (kernel time on the CPU charts) is layered on top of the
// primary one with the same colour, so where both overlap the fill darkens.
class PerfGraph : public QWidget
{
public:
    explicit PerfGraph(const QColor &color, QWidget *parent = nullptr);

    void setGrid(int columns, int rows);
    void setCapacity(int samples);
    void setMaximum(double maximum);
    // Enables the primary-series hover marker; an empty unit disables it.
    void setHoverUnit(const QString &unit);

    void appendValue(double value);
    void appendSecondaryValue(double value);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QColor m_color;
    int m_columns = 10;
    int m_rows = 10;
    int m_capacity = 60;
    double m_maximum = 100.0;
    QVector<double> m_values;
    QVector<double> m_secondary;
    QString m_hoverUnit;
    qreal m_hoverX = -1.0;

    void trim(QVector<double> &values) const;
    QPointF samplePoint(int index, const QVector<double> &values) const;
    void drawSeries(QPainter &painter, const QVector<double> &values, int fillAlpha) const;
    void drawHover(QPainter &painter) const;
};
