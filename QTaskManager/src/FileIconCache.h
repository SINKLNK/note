#pragma once

#include <QByteArray>
#include <QHash>
#include <QIcon>
#include <QString>

// Shell icons of files, fetched once per path and kept as plain pixmaps.
//
// The QIcon that QFileIconProvider hands out is only a handle. For .exe, .lnk
// and .ico files Qt deliberately keeps no pixmap behind it (an empty
// QWindowsFileIconEngine::cacheKey()), so every paint of such an icon went
// back to SHGetFileInfo: a shell parse of the path, a file system lookup,
// overlay handlers, zone checks. Holding on to that QIcon cached nothing; a
// profile of the process list had a third of the app's CPU in it. This keeps
// the pixmaps themselves. An executable whose icon changes on disk while we
// run goes on showing the old one.
//
// Not thread-safe, like QFileIconProvider itself: GUI thread only.
class FileIconCache
{
public:
    QIcon icon(const QString &path);

private:
    QHash<QString, QIcon> m_byPath;
    // Many paths come out with the very same picture -- the generic exe icon
    // above all -- so identical pixmaps are kept once and shared.
    QHash<QByteArray, QIcon> m_byContent;
};
