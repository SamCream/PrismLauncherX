// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include "tasks/Task.h"

class DownloadAuthlibInjector : public Task {
    Q_OBJECT
   public:
    explicit DownloadAuthlibInjector(QNetworkAccessManager* network = nullptr,
                                     const QString& cacheRoot = {},
                                     const QUrl& metadata = QUrl("https://authlib-injector.yushi.moe/artifact/latest.json"));
    ~DownloadAuthlibInjector() override;
    static QString jarPath();
    static bool verify(const QByteArray& jar, const QJsonObject& metadata);
    bool abort() override;
    bool canAbort() const override { return true; }

   protected:
    void executeTask() override;

   private:
    void fetch(const QUrl& url, bool artifact);
    QNetworkAccessManager* m_network;
    QString m_root;
    QUrl m_metadataURL;
    QJsonObject m_metadata;
    QPointer<QNetworkReply> m_reply;
};
