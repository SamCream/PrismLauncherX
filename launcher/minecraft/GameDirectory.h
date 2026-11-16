// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QDir>
#include <QFileInfo>

namespace GameDirectory {
// 0 shared, 1 isolated, 2 isolate modded, 3 custom, 4 legacy layout.
inline QString resolve(const QString& instance, const QString& shared, int mode, bool modded, const QString& custom)
{
    if (mode == 4) {
        const auto plain = QDir(instance).filePath("minecraft");
        const auto dotted = QDir(instance).filePath(".minecraft");
        return QFileInfo::exists(dotted) && !QFileInfo::exists(plain) ? dotted : plain;
    }
    if (mode == 3 && !custom.trimmed().isEmpty())
        return QDir(instance).absoluteFilePath(custom.trimmed());
    if (mode == 0 || (mode == 2 && !modded))
        return shared;
    return instance;
}
}  // namespace GameDirectory
