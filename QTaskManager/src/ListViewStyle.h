#pragma once

#include <QString>

// The look of the item views that list processes: flat white rows, light
// separators, slim scroll bars. Shared so the pages stay visually identical.
inline QString listViewStyleSheet(bool itemSeparators = true)
{
    return QStringLiteral(R"(
    QTreeView {
        background: white;
        border: 1px solid #d9d9d9;
        border-top: none;
        alternate-background-color: white;
        selection-background-color: #d6e9ff;
        selection-color: black;
        gridline-color: #ececec;
        show-decoration-selected: 1;
    }
    QHeaderView::section {
        background: #fafafa;
        color: #111;
        border: 0;
        border-right: 1px solid #ececec;
        border-bottom: 1px solid #d9d9d9;
        padding: 6px 8px;
        font-weight: 500;
    }
    QTreeView::item {
        height: %2px;
        %1
    }
    QScrollBar:vertical {
        background: #f1f1f1;
        width: 16px;
        margin: 0;
        border-left: 1px solid #dddddd;
    }
    QScrollBar::handle:vertical {
        background: #8d8d8d;
        min-height: 42px;
        margin: 2px 3px;
        border-radius: 5px;
    }
    QScrollBar::handle:vertical:hover {
        background: #686868;
    }
    QScrollBar::handle:vertical:pressed {
        background: #505050;
    }
    QScrollBar:horizontal {
        background: #f1f1f1;
        height: 16px;
        margin: 0;
        border-top: 1px solid #dddddd;
    }
    QScrollBar::handle:horizontal {
        background: #8d8d8d;
        min-width: 42px;
        margin: 3px 2px;
        border-radius: 5px;
    }
    QScrollBar::handle:horizontal:hover {
        background: #686868;
    }
    QScrollBar::handle:horizontal:pressed {
        background: #505050;
    }
    QScrollBar::add-line, QScrollBar::sub-line {
        width: 0;
        height: 0;
        border: none;
        background: none;
    }
    QScrollBar::add-page, QScrollBar::sub-page {
        background: transparent;
    }
)").arg(itemSeparators ? QStringLiteral("border-bottom: 1px solid #f2f2f2;") : QString())
        // The QSS border used to add one pixel to the row's total height.
        .arg(itemSeparators ? 26 : 27);
}
