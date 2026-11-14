#include "FileIconCache.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QImage>
#include <QPixmap>
#include <QStyle>

namespace
{
// The views draw these icons at 20 px. The shell keeps 32 and 48 px images
// (SHGFI_LARGEICON, SHIL_EXTRALARGE), which between them cover 20 px up to
// 240% scaling; QIcon picks the nearest larger one and scales it down once,
// smoothly, into QPixmapCache. The 16 px image would never be picked for 20 px.
constexpr int kIconSides[] = {32, 48};
}

QIcon FileIconCache::icon(const QString &path)
{
    const auto cached = m_byPath.constFind(path);
    if (cached != m_byPath.cend()) {
        return cached.value();
    }

    QIcon icon;
    if (path.isEmpty()) {
        icon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    } else {
        const QIcon shellIcon = QFileIconProvider().icon(QFileInfo(path));
        QCryptographicHash content(QCryptographicHash::Sha1);
        for (const int side : kIconSides) {
            const QPixmap pixmap = shellIcon.pixmap(QSize(side, side), 1.0);
            if (pixmap.isNull()) {
                continue;
            }
            const QImage image = pixmap.toImage();
            const int header[] = {image.width(), image.height(), int(image.format())};
            content.addData(QByteArrayView(reinterpret_cast<const char *>(header), sizeof(header)));
            content.addData(QByteArrayView(reinterpret_cast<const char *>(image.constBits()),
                                           image.sizeInBytes()));
            // The shell's engine ignores the mode, so a selected row showed its
            // icon as it is. A pixmap icon left without pixmaps for a mode makes
            // them up, and for Selected that means tinting with the highlight.
            // The same pixmaps for every mode the views use keep it unchanged;
            // Disabled still gets greyed out.
            for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Active, QIcon::Selected}) {
                icon.addPixmap(pixmap, mode);
            }
        }

        if (icon.isNull()) {
            icon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
        } else {
            const QByteArray key = content.result();
            const auto same = m_byContent.constFind(key);
            if (same != m_byContent.cend()) {
                icon = same.value();
            } else {
                m_byContent.insert(key, icon);
            }
        }
    }

    m_byPath.insert(path, icon);
    return icon;
}
