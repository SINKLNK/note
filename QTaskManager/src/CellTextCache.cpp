#include "CellTextCache.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTextLayout>

#include <limits>

CellTextCache::CellTextCache(qsizetype capacity)
    : m_capacity(qMax<qsizetype>(capacity, 1))
{
}

bool CellTextCache::paint(QPainter *painter, const QStyleOptionViewItem &option)
{
    if ((option.features & QStyleOptionViewItem::WrapText)
        || (option.state & QStyle::State_HasFocus)
        || option.text.contains(QChar::LineSeparator)
        || option.text.contains(QChar::ParagraphSeparator)) {
        return false;
    }

    const QWidget *widget = option.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();

    // Background, selection and decoration come from the style as usual, just
    // without the text. Without HasDisplay the style lays the cell out exactly
    // as it would with it -- the text rect gets whatever room is left -- but no
    // longer shapes the text merely to measure it.
    QStyleOptionViewItem bare = option;
    bare.text.clear();
    bare.features &= ~QStyleOptionViewItem::HasDisplay;
    style->drawControl(QStyle::CE_ItemViewItem, &bare, painter, widget);
    if (option.text.isEmpty()) {
        return true;
    }

    // The rest is what QCommonStyle's CE_ItemViewItem and viewItemDrawText()
    // do with the text, minus laying it out every time.
    const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, widget) + 1;
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &bare, widget)
                               .adjusted(margin, 0, -margin, 0);
    const Entry &entry = entryFor(option, textRect);

    QPalette::ColorGroup group = (option.state & QStyle::State_Enabled) ? QPalette::Normal
                                                                        : QPalette::Disabled;
    if (group == QPalette::Normal && !(option.state & QStyle::State_Active)) {
        group = QPalette::Inactive;
    }

    painter->save();
    painter->setClipRect(option.rect, Qt::IntersectClip);
    // The colour comes from the palette, as in the style. A style sheet that
    // gave ::item a color of its own would not reach this.
    painter->setPen(option.palette.color(group,
                                         (option.state & QStyle::State_Selected)
                                             ? QPalette::HighlightedText
                                             : QPalette::Text));
    entry.layout->draw(painter, textRect.topLeft() + entry.offset);
    painter->restore();
    return true;
}

const CellTextCache::Entry &CellTextCache::entryFor(const QStyleOptionViewItem &option,
                                                    const QRect &textRect)
{
    const Key key{option.text, textRect.size()};
    const auto matches = [&option](const Entry &entry) {
        return entry.font == option.font && entry.alignment == option.displayAlignment
               && entry.direction == option.direction && entry.elideMode == option.textElideMode;
    };

    const auto current = m_current.constFind(key);
    if (current != m_current.cend() && matches(*current)) {
        return *current;
    }

    Entry entry;
    const auto previous = m_previous.constFind(key);
    if (previous != m_previous.cend() && matches(*previous)) {
        entry = *previous;
    } else {
        entry = layOut(option, textRect);
    }

    if (m_current.size() >= m_capacity) {
        m_previous.swap(m_current);
        m_current.clear();
    }
    return *m_current.insert(key, std::move(entry));
}

CellTextCache::Entry CellTextCache::layOut(const QStyleOptionViewItem &option, const QRect &textRect)
{
    QTextOption textOption;
    textOption.setWrapMode(QTextOption::ManualWrap);
    textOption.setTextDirection(option.direction);
    textOption.setAlignment(QStyle::visualAlignment(option.direction, option.displayAlignment));

    auto layout = std::make_shared<QTextLayout>(
        elided(option.text, option.font, option.textElideMode, textRect.width()), option.font);
    layout->setTextOption(textOption);
    // Keeps the shaped glyphs past endLayout(); without it draw() shapes again.
    layout->setCacheEnabled(true);
    layout->beginLayout();
    QTextLine line = layout->createLine();
    if (line.isValid()) {
        line.setLineWidth(textRect.width());
        line.setPosition(QPointF(0, 0));
    }
    layout->endLayout();

    // Placed vertically the way QCommonStylePrivate::calculateElidedText() does.
    const QRect layoutRect = QStyle::alignedRect(Qt::LayoutDirectionAuto,
                                                 option.displayAlignment,
                                                 layout->boundingRect().size().toSize(),
                                                 textRect);

    Entry entry;
    entry.font = option.font;
    entry.alignment = option.displayAlignment;
    entry.direction = option.direction;
    entry.elideMode = option.textElideMode;
    entry.layout = std::move(layout);
    entry.offset = QPointF(0, layoutRect.top() - textRect.top());
    return entry;
}

QString CellTextCache::elided(const QString &text, const QFont &font, Qt::TextElideMode mode, int width)
{
    const QFontMetrics metrics(font);

    // Eliding shapes the whole string, and a command line can run to a couple
    // of thousand characters where a few hundred fill the widest column. Hand
    // it a prefix that cannot fit even if made of the narrowest glyphs only:
    // whenever the prefix does not fit, the elision point lies inside it and
    // the result is the same. Should the whole prefix fit after all (glyphs
    // narrower than any of ASCII), elide the full text instead.
    if (mode == Qt::ElideRight) {
        const qsizetype enough = qsizetype(width / narrowestAdvance(font)) + 2;
        if (text.size() > enough) {
            const QString prefix = text.left(enough);
            const QString result = metrics.elidedText(prefix, mode, width);
            if (result != prefix) {
                return result;
            }
        }
    }

    return metrics.elidedText(text, mode, width);
}

qreal CellTextCache::narrowestAdvance(const QFont &font)
{
    if (m_narrowest > 0.0 && m_narrowestFont == font) {
        return m_narrowest;
    }

    const QFontMetricsF metrics(font);
    qreal narrowest = std::numeric_limits<qreal>::max();
    for (char16_t c = u' '; c <= u'~'; ++c) {
        narrowest = qMin(narrowest, metrics.horizontalAdvance(QChar(c)));
    }

    m_narrowestFont = font;
    m_narrowest = qMax(narrowest, 1.0);
    return m_narrowest;
}
