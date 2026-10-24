// SPDX-License-Identifier: GPL-3.0-only
#include "DownloadAuthlibInjector.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include "Application.h"
#include "minecraft/auth/Yggdrasil.h"

DownloadAuthlibInjector::DownloadAuthlibInjector(QNetworkAccessManager* network, const QString& root, const QUrl& metadata)
    : m_network(network), m_root(root), m_metadataURL(metadata)
{}
DownloadAuthlibInjector::~DownloadAuthlibInjector()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
    }
}
QString DownloadAuthlibInjector::jarPath()
{
    return QDir(APPLICATION->dataRoot()).filePath("authlib-injector/authlib-injector.jar");
}
bool DownloadAuthlibInjector::verify(const QByteArray& jar, const QJsonObject& metadata)
{
    const auto hash = metadata.value("checksums").toObject().value("sha256").toString().toLatin1().toLower();
    static const QRegularExpression sha("^[0-9a-f]{64}$");
    return !jar.isEmpty() && sha.match(QString::fromLatin1(hash)).hasMatch() &&
           QCryptographicHash::hash(jar, QCryptographicHash::Sha256).toHex() == hash;
}
void DownloadAuthlibInjector::executeTask()
{
    if (m_root.isEmpty())
        m_root = QFileInfo(jarPath()).absolutePath();
    if (!m_network)
        m_network = APPLICATION->network();
    QFile metadata(QDir(m_root).filePath("artifact.json"));
    QFile jar(QDir(m_root).filePath("authlib-injector.jar"));
    if (metadata.open(QIODevice::ReadOnly) && jar.open(QIODevice::ReadOnly) &&
        verify(jar.readAll(), QJsonDocument::fromJson(metadata.readAll()).object())) {
        emitSucceeded();
        return;
    }
    setStatus(tr("下载并校验 authlib-injector"));
    fetch(m_metadataURL, false);
}
void DownloadAuthlibInjector::fetch(const QUrl& url, bool artifact)
{
    if (!Yggdrasil::validServer(url)) {
        emitFailed(tr("authlib-injector 下载地址无效或不安全。"));
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) { setProgress(received, total); });
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (m_reply->bytesAvailable() > 32 * 1024 * 1024)
            m_reply->abort();
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, artifact] {
        const auto error = m_reply->error();
        const auto status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto bytes = m_reply->readAll();
        m_reply->deleteLater();
        m_reply.clear();
        if (error != QNetworkReply::NoError || status != 200) {
            emitFailed(tr("无法下载 authlib-injector（HTTP %1），请检查网络后重试。").arg(status));
            return;
        }
        if (!artifact) {
            m_metadata = QJsonDocument::fromJson(bytes).object();
            const QUrl download(m_metadata.value("download_url").toString());
            if (!Yggdrasil::validServer(download) || m_metadata.value("checksums").toObject().value("sha256").toString().size() != 64) {
                emitFailed(tr("authlib-injector 发布元数据无效。"));
                return;
            }
            fetch(download, true);
            return;
        }
        if (!verify(bytes, m_metadata)) {
            emitFailed(tr("authlib-injector SHA-256 校验失败，已拒绝使用此文件。"));
            return;
        }
        if (!QDir().mkpath(m_root)) {
            emitFailed(tr("无法创建 authlib-injector 缓存目录。"));
            return;
        }
        QSaveFile jar(QDir(m_root).filePath("authlib-injector.jar"));
        QSaveFile metadata(QDir(m_root).filePath("artifact.json"));
        const auto json = QJsonDocument(m_metadata).toJson();
        if (!jar.open(QIODevice::WriteOnly) || jar.write(bytes) != bytes.size() || !jar.commit() || !metadata.open(QIODevice::WriteOnly) ||
            metadata.write(json) != json.size() || !metadata.commit()) {
            emitFailed(tr("无法保存 authlib-injector 缓存。"));
            return;
        }
        emitSucceeded();
    });
}
bool DownloadAuthlibInjector::abort()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply.clear();
    }
    emitAborted();
    return true;
}
