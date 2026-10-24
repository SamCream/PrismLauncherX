// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QRegularExpression>
#include <QUrl>
namespace Net::DownloadSource {
inline bool validBase(const QUrl& base)
{
    return base.isValid() && base.scheme() == "https" && !base.host().isEmpty() && base.userInfo().isEmpty() && !base.hasQuery() &&
           !base.hasFragment();
}
inline QUrl rewrite(const QUrl& original, const QString& source, const QUrl& custom)
{
    if (source == "official" || original.scheme() != "https" || !original.userInfo().isEmpty())
        return original;
    QUrl base(source == "bmclapi" ? QUrl("https://bmclapi2.bangbang93.com") : custom);
    if ((source != "bmclapi" && source != "custom") || !validBase(base))
        return original;
    const auto host = original.host().toLower();
    QString prefix;
    if (host == "resources.download.minecraft.net")
        prefix = "/assets";
    else if (host == "libraries.minecraft.net" || host == "maven.minecraftforge.net" || host == "maven.neoforged.net")
        prefix = "/maven";
    else if (host != "piston-data.mojang.com" && host != "piston-meta.mojang.com" && host != "launchermeta.mojang.com" &&
             host != "launcher.mojang.com")
        return original;
    base.setPath(base.path().remove(QRegularExpression("/+$")) + prefix + original.path());
    base.setQuery(original.query());
    return base;
}
}  // namespace Net::DownloadSource
