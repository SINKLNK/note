#pragma once

#include <QFont>
#include <QHash>
#include <QPointF>
#include <QSize>
#include <QString>

#include <memory>

class QPainter;
class QRect;
class QStyleOptionViewItem;
class QTextLayout;

// Draws the text of item view cells from layouts kept between paints.
//
// QStyledItemDelegate lays a cell's text out from scratch on every paint, and
// not once: the style shapes it to measure the cell, again to elide it and a
// third time to draw it -- the whole string each time, however little of it
// fits. For command lines of a couple of thousand characters, repainted every
// second and on every step of a scroll, that came to the better part of a
// third of the app's CPU.
// Here the elided text is laid out once per text and cell size and drawn from
// that layout until the cell changes size, on a column resize for instance.
//
// Every distinct text is an entry of its own. A column of figures that change
// every second still gains: between two ticks a scroll repaints the same few
// dozen strings over and over, and many repeat across rows ("0%").
class CellTextCache
{
public:
    // How many entries to keep: comfortably more than a screenful of the
    // column's cells, or scrolling back and forth lays them out again.
    explicit CellTextCache(qsizetype capacity = 256);

    // Paints `option`, already filled in by initStyleOption(), the way
    // QStyledItemDelegate::paint() would. Returns false, having painted
    // nothing, for a cell it leaves to the style: wrapped or multi-line text,
    // or one that draws its own focus frame.
    bool paint(QPainter *painter, const QStyleOptionViewItem &option);

private:
    // The size is part of the key, not just checked against: the same text
    // comes in several widths at once -- a process name at different depths
    // of the tree, a cell narrowed while its button shows.
    struct Key
    {
        QString text;
        QSize size; // of the text rect the layout is made for
        friend bool operator==(const Key &, const Key &) = default;
        friend size_t qHash(const Key &key, size_t seed = 0)
        {
            return qHashMulti(seed, key.text, key.size.width(), key.size.height());
        }
    };

    struct Entry
    {
        QFont font;
        Qt::Alignment alignment;
        Qt::LayoutDirection direction = Qt::LeftToRight;
        Qt::TextElideMode elideMode = Qt::ElideRight;
        std::shared_ptr<QTextLayout> layout;
        QPointF offset; // where to draw the layout, relative to the text rect
    };

    const Entry &entryFor(const QStyleOptionViewItem &option, const QRect &textRect);
    Entry layOut(const QStyleOptionViewItem &option, const QRect &textRect);
    QString elided(const QString &text, const QFont &font, Qt::TextElideMode mode, int width);
    qreal narrowestAdvance(const QFont &font);

    // Two generations instead of a least-recently-used list: entries go into
    // the current one; once it is full it becomes the previous one, and the
    // one before is dropped. An entry still in use gets moved back into the
    // current generation the next time it is drawn, so whatever is on screen
    // survives the turnover.
    qsizetype m_capacity;
    QHash<Key, Entry> m_current;
    QHash<Key, Entry> m_previous;

    QFont m_narrowestFont;
    qreal m_narrowest = 0.0;
};
