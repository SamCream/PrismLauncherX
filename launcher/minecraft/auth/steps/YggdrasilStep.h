// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QNetworkAccessManager>
#include <QPointer>
#include "minecraft/auth/AuthStep.h"
class YggdrasilStep : public AuthStep {
    Q_OBJECT
   public:
    YggdrasilStep(AccountData* data, bool refresh, QNetworkAccessManager* network = nullptr);
    ~YggdrasilStep() override;
    QString describe() override { return tr("正在验证皮肤站账号"); }
    void perform() override;
    void abort() override;

   private:
    bool m_refresh;
    QNetworkAccessManager* m_network;
    QPointer<QNetworkReply> m_reply;
};
