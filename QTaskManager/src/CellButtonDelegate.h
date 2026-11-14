#pragma once

#include "CellTextCache.h"

#include <QIcon>
#include <QPersistentModelIndex>
#include <QStyledItemDelegate>

class QTimer;
class QTreeView;

// Draws a small button at the left or right edge of the cell the mouse is
// over, in one column, and reports clicks on it. What the button does is up to
// whoever listens to clicked(): copying the command line, trimming the working
// set.
//
// By default the button is there as soon as the mouse is. setShowDelay() makes
// it wait: it appears only once the mouse has stayed in the same cell that
// long, and is neither drawn nor clickable before that.
class CellButtonDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    CellButtonDelegate(QTreeView *view,
                       int column,
                       const QString &iconPath,
                       const QString &toolTip,
                       Qt::Edge edge,
                       QObject *parent = nullptr);

    void setShowDelay(int milliseconds);

    // Draws the cell text from a layout kept between paints (CellTextCache)
    // instead of laying it out on every paint. Worth it for long text that
    // seldom changes, such as the command line.
    void setCachesText(bool enabled);

signals:
    void clicked(const QModelIndex &index);

protected:
    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    bool helpEvent(QHelpEvent *event,
                   QAbstractItemView *view,
                   const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QRect buttonRect(const QRect &cellRect) const;
    bool buttonHit(const QPoint &viewportPos) const;
    void setHovered(const QModelIndex &index, bool overButton);
    bool overVisibleButton() const;

    QTreeView *m_view = nullptr;
    int m_column = 0;
    QIcon m_icon;
    QString m_toolTip;
    Qt::Edge m_edge = Qt::RightEdge;

    // Persistent, so the button stays on the process it was pointing at even
    // when the row moves under it during a refresh.
    QPersistentModelIndex m_hovered;
    // The mouse is over the spot where the button is, shown yet or not.
    bool m_overButton = false;
    bool m_pressed = false;

    int m_showDelayMs = 0;
    QTimer *m_showTimer = nullptr;
    bool m_buttonVisible = false;

    bool m_cachesText = false;
    mutable CellTextCache m_textCache;
};
