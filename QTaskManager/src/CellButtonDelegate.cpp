#include "CellButtonDelegate.h"

#include <QHelpEvent>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolTip>
#include <QTreeView>

namespace
{
constexpr int kButtonSize = 18;
constexpr int kButtonMargin = 4;
constexpr int kTextGap = kButtonSize + kButtonMargin * 2;
}

CellButtonDelegate::CellButtonDelegate(QTreeView *view,
                                       int column,
                                       const QString &iconPath,
                                       const QString &toolTip,
                                       Qt::Edge edge,
                                       QObject *parent)
    : QStyledItemDelegate(parent),
      m_view(view),
      m_column(column),
      m_icon(iconPath),
      m_toolTip(toolTip),
      // Only the two horizontal edges make sense inside a row.
      m_edge(edge == Qt::LeftEdge ? Qt::LeftEdge : Qt::RightEdge)
{
    // The view reports hover per row, not per cell, so the column under the
    // cursor is tracked here instead of relying on State_MouseOver. Several
    // delegates may filter the same viewport; each one only reacts to its own
    // column, so they never fight over a hover or a press.
    m_view->viewport()->setMouseTracking(true);
    m_view->viewport()->installEventFilter(this);

    m_showTimer = new QTimer(this);
    m_showTimer->setSingleShot(true);
    connect(m_showTimer, &QTimer::timeout, this, [this] {
        m_buttonVisible = m_hovered.isValid();
        if (m_buttonVisible) {
            m_view->viewport()->update(m_view->visualRect(m_hovered));
        }
    });
}

void CellButtonDelegate::setShowDelay(int milliseconds)
{
    m_showDelayMs = qMax(0, milliseconds);
}

void CellButtonDelegate::setCachesText(bool enabled)
{
    m_cachesText = enabled;
}

bool CellButtonDelegate::overVisibleButton() const
{
    return m_buttonVisible && m_overButton;
}

QRect CellButtonDelegate::buttonRect(const QRect &cellRect) const
{
    const int size = qMin(kButtonSize, cellRect.height() - 2);
    const int left = m_edge == Qt::LeftEdge ? cellRect.left() + kButtonMargin
                                            : cellRect.right() - size - kButtonMargin;
    return QRect(left,
                 cellRect.top() + (cellRect.height() - size) / 2,
                 size,
                 size);
}

bool CellButtonDelegate::buttonHit(const QPoint &viewportPos) const
{
    return m_hovered.isValid() && m_buttonVisible
           && buttonRect(m_view->visualRect(m_hovered)).contains(viewportPos);
}

void CellButtonDelegate::setHovered(const QModelIndex &index, bool overButton)
{
    if (QModelIndex(m_hovered) == index && m_overButton == overButton) {
        return;
    }

    const bool newCell = QModelIndex(m_hovered) != index;

    if (m_hovered.isValid()) {
        m_view->viewport()->update(m_view->visualRect(m_hovered));
    }

    m_hovered = index;
    m_overButton = overButton;

    // Moving within a cell leaves the wait running; a different cell, or
    // leaving the column, drops the button at once and starts the wait over.
    if (newCell) {
        m_showTimer->stop();
        m_buttonVisible = index.isValid() && m_showDelayMs == 0;
        if (index.isValid() && m_showDelayMs > 0) {
            m_showTimer->start(m_showDelayMs);
        }
    }

    if (m_hovered.isValid()) {
        m_view->viewport()->update(m_view->visualRect(m_hovered));
    }
}

void CellButtonDelegate::paint(QPainter *painter,
                               const QStyleOptionViewItem &option,
                               const QModelIndex &index) const
{
    const bool showButton = m_buttonVisible && m_hovered.isValid()
                            && QModelIndex(m_hovered) == index;
    QStyleOptionViewItem textOption = option;
    if (showButton) {
        // Narrow the text rect so the elided text stops before the button
        // rather than running underneath it.
        if (m_edge == Qt::LeftEdge) {
            textOption.rect.setLeft(textOption.rect.left() + kTextGap);
        } else {
            textOption.rect.setRight(textOption.rect.right() - kTextGap);
        }
    }

    // Shell icons come as 16/32/48/256 px. FileIconCache scales them down to
    // the 20 px the view asks for itself, but an icon that hands back the 32 px
    // image tagged with a device pixel ratio instead -- as the shell's own icon
    // engine does -- is drawn by the item view without smoothing: nearest-
    // neighbour 32 -> 20 drops 12 of every 32 rows and columns. A full-colour exe
    // icon survives it, but the 1 px outline of the generic "no icon" page does
    // not -- its top and bottom edges vanish and it looks cut off by the row.
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    bool painted = false;
    if (m_cachesText) {
        QStyleOptionViewItem cellOption = textOption;
        initStyleOption(&cellOption, index);
        painted = m_textCache.paint(painter, cellOption);
    }
    if (!painted) {
        QStyledItemDelegate::paint(painter, textOption, index);
    }
    painter->restore();

    if (!showButton) {
        return;
    }

    const QRect rect = buttonRect(option.rect);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    if (m_pressed && overVisibleButton()) {
        painter->setBrush(QColor(0xc2, 0xdb, 0xf8));
    } else if (overVisibleButton()) {
        painter->setBrush(QColor(0xe4, 0xee, 0xfb));
    } else {
        painter->setBrush(QColor(0, 0, 0, 16));
    }
    painter->drawRoundedRect(rect, 4, 4);
    painter->restore();

    m_icon.paint(painter, rect.adjusted(2, 2, -2, -2));
}

bool CellButtonDelegate::helpEvent(QHelpEvent *event,
                                   QAbstractItemView *view,
                                   const QStyleOptionViewItem &option,
                                   const QModelIndex &index)
{
    if (event && event->type() == QEvent::ToolTip && overVisibleButton()
        && m_hovered.isValid() && QModelIndex(m_hovered) == index) {
        QToolTip::showText(event->globalPos(), m_toolTip, view);
        return true;
    }

    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

bool CellButtonDelegate::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_view->viewport()) {
        return QStyledItemDelegate::eventFilter(watched, event);
    }

    switch (event->type()) {
    // The item view puts Qt::WA_Hover on its viewport, so plain mouse motion
    // arrives as HoverMove and a MouseMove-only filter never fires without a
    // button held down. Both carry a position; in Qt 6 both are
    // QSinglePointEvent, so one branch handles them.
    case QEvent::MouseMove:
    case QEvent::HoverMove: {
        const auto *pointer = static_cast<QSinglePointEvent *>(event);
        const QPoint pos = pointer->position().toPoint();
        const QModelIndex index = m_view->indexAt(pos);
        const bool onColumn = index.isValid() && index.column() == m_column;
        setHovered(onColumn ? index : QModelIndex(),
                   onColumn && buttonRect(m_view->visualRect(index)).contains(pos));
        break;
    }

    case QEvent::Leave:
    case QEvent::HoverLeave:
        setHovered(QModelIndex(), false);
        break;

    case QEvent::MouseButtonPress: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton || !buttonHit(mouse->position().toPoint())) {
            break;
        }

        m_pressed = true;
        m_view->viewport()->update(m_view->visualRect(m_hovered));
        // Swallowed: pressing the button must not move the selection.
        return true;
    }

    case QEvent::MouseButtonRelease: {
        if (!m_pressed) {
            break;
        }

        m_pressed = false;
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const bool onButton = buttonHit(mouse->position().toPoint());
        const QModelIndex hovered = m_hovered;

        if (hovered.isValid()) {
            m_view->viewport()->update(m_view->visualRect(hovered));
        }

        if (onButton && hovered.isValid()) {
            emit clicked(hovered);
        }

        return true;
    }

    default:
        break;
    }

    return QStyledItemDelegate::eventFilter(watched, event);
}
