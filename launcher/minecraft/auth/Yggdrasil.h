// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include "AccountData.h"
namespace Yggdrasil {
inline bool validServer(const QUrl& url)
{
    const bool local = url.host() == "127.0.0.1" || url.host() == "localhost" || url.host() == "::1";
    return url.isValid() && !url.host().isEmpty() && (url.scheme() == "https" || (local && url.scheme() == "http")) &&
           url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
}
inline QString normalizedServer(QUrl url)
{
    url.setPath(url.path().remove(QRegularExpression("/+$")) + "/");
    return url.toString();
}
inline bool profile(const QJsonObject& json, MinecraftProfile& out)
{
    const auto id = json.value("id").toString();
    const auto name = json.value("name").toString();
    static const QRegularExpression uuid("^[0-9a-fA-F]{32}$");
    if (!uuid.match(id).hasMatch() || name.isEmpty() || name.size() > 16)
        return false;
    out.id = id;
    out.name = name;
    out.validity = Validity::Certain;
    return true;
}
}  // namespace Yggdrasil
