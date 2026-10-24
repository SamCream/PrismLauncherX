// SPDX-License-Identifier: GPL-3.0-only
#include "YggdrasilStep.h"
#include <QJsonDocument>
#include <QUuid>
#include "Application.h"
#include "minecraft/auth/Yggdrasil.h"

YggdrasilStep::YggdrasilStep(AccountData* data, bool refresh, QNetworkAccessManager* network)
    : AuthStep(data), m_refresh(refresh), m_network(network)
{}
YggdrasilStep::~YggdrasilStep()
{
    abort();
}
void YggdrasilStep::abort()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply.clear();
    }
    m_data->authPassword.clear();
}
void YggdrasilStep::perform()
{
    if (!Yggdrasil::validServer(QUrl(m_data->authServer))) {
        m_data->authPassword.clear();
        emit finished(AccountTaskState::STATE_FAILED_SOFT, tr("皮肤站地址无效，请使用 HTTPS API 地址。"));
        return;
    }
    auto client = m_data->yggdrasilToken.extra.value("clientToken").toString();
    if (client.isEmpty())
        client = QUuid::createUuid().toString(QUuid::Id128);
    QJsonObject body{ { "clientToken", client }, { "requestUser", true } };
    if (m_refresh) {
        body["accessToken"] = m_data->accessToken();
        if (!m_data->minecraftProfile.id.isEmpty())
            body["selectedProfile"] = QJsonObject{ { "id", m_data->minecraftProfile.id }, { "name", m_data->minecraftProfile.name } };
    } else {
        body["agent"] = QJsonObject{ { "name", "Minecraft" }, { "version", 1 } };
        body["username"] = m_data->authUsername;
        body["password"] = m_data->authPassword;
    }
    m_data->authPassword.clear();
    const QUrl url(Yggdrasil::normalizedServer(QUrl(m_data->authServer)) + (m_refresh ? "authserver/refresh" : "authserver/authenticate"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // Never forward credentials to a redirect destination.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    auto network = m_network ? m_network : APPLICATION->network();
    m_reply = network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    const auto expectedProfile = m_refresh ? m_data->minecraftProfile.id : QString();
    connect(m_reply, &QNetworkReply::finished, this, [this, client, expectedProfile] {
        const auto status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = m_reply->error();
        const auto bytes = m_reply->readAll();
        m_reply->deleteLater();
        m_reply.clear();
        if (error != QNetworkReply::NoError || status != 200) {
            const auto state = status == 401 || status == 403 ? AccountTaskState::STATE_FAILED_HARD
                               : status == 0 || status >= 500 ? AccountTaskState::STATE_OFFLINE
                                                              : AccountTaskState::STATE_FAILED_SOFT;
            emit finished(
                state, tr("皮肤站认证失败（HTTP %1）。请检查账号、密码、API 地址和网络。支持二步验证的站点请使用其应用密码。").arg(status));
            return;
        }
        const auto json = QJsonDocument::fromJson(bytes).object();
        const auto token = json.value("accessToken").toString();
        const auto returnedClient = json.value("clientToken").toString();
        if (token.isEmpty() || returnedClient != client) {
            emit finished(AccountTaskState::STATE_FAILED_SOFT, tr("皮肤站返回了无效的令牌。"));
            return;
        }
        auto profiles = json.value("availableProfiles").toArray();
        auto selected = json.value("selectedProfile").toObject();
        if (selected.isEmpty() && profiles.size() == 1)
            selected = profiles.first().toObject();
        if (!selected.isEmpty() && ((!expectedProfile.isEmpty() && selected.value("id").toString() != expectedProfile) ||
                                    !Yggdrasil::profile(selected, m_data->minecraftProfile))) {
            emit finished(AccountTaskState::STATE_FAILED_SOFT, tr("皮肤站返回了无效的角色信息。"));
            return;
        }
        if (!m_refresh && selected.isEmpty() && profiles.isEmpty()) {
            emit finished(AccountTaskState::STATE_FAILED_SOFT, tr("此账号没有角色，请先在皮肤站创建角色。"));
            return;
        }
        m_data->availableProfiles = profiles;
        auto& auth = m_data->yggdrasilToken;
        auth.token = token;
        auth.extra["clientToken"] = client;
        auth.issueInstant = QDateTime::currentDateTimeUtc();
        auth.notAfter = auth.issueInstant.addSecs(24 * 3600);
        auth.validity = Validity::Certain;
        // A third-party identity is not a Microsoft purchase entitlement.
        m_data->minecraftEntitlement.canPlayMinecraft = true;
        m_data->minecraftEntitlement.ownsMinecraft = false;
        emit finished(AccountTaskState::STATE_WORKING, tr("皮肤站认证成功"));
    });
}
