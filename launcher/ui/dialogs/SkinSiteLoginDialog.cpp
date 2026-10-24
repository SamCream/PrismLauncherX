// SPDX-License-Identifier: GPL-3.0-only
#include "SkinSiteLoginDialog.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include "minecraft/auth/AuthFlow.h"
#include "minecraft/auth/Yggdrasil.h"
#include "ui/dialogs/ProgressDialog.h"

MinecraftAccountPtr SkinSiteLoginDialog::newAccount(QWidget* parent, const QString& server, const QString& username)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("添加皮肤站账号 / LittleSkin"));
    auto layout = new QFormLayout(&dialog);
    auto address = new QLineEdit(server, &dialog);
    auto user = new QLineEdit(username, &dialog);
    auto password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    layout->addRow(QObject::tr("皮肤站 API 地址"), address);
    layout->addRow(QObject::tr("邮箱 / 用户名"), user);
    layout->addRow(QObject::tr("密码 / 应用密码"), password);
    auto note = new QLabel(
        QObject::tr(
            "默认兼容 LittleSkin，也支持 authlib-injector/Yggdrasil 皮肤站。密码不会保存；启用二步验证时请使用站点提供的应用密码。"),
        &dialog);
    note->setWordWrap(true);
    layout->addRow(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!Yggdrasil::validServer(QUrl(address->text().trimmed())) || user->text().trimmed().isEmpty() || password->text().isEmpty()) {
            QMessageBox::warning(&dialog, QObject::tr("无法登录"), QObject::tr("请输入有效的 HTTPS API 地址、用户名和密码。"));
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return nullptr;
    auto account = MinecraftAccount::createBlankMSA();
    auto data = account->accountData();
    data->type = AccountType::Yggdrasil;
    data->authServer = Yggdrasil::normalizedServer(QUrl(address->text().trimmed()));
    data->authUsername = user->text().trimmed();
    data->authPassword = password->text();
    password->clear();
    auto task = account->login();
    ProgressDialog progress(parent);
    progress.execWithTask(task.get());
    if (!task->wasSuccessful())
        return nullptr;
    if (data->minecraftProfile.id.isEmpty()) {
        QStringList names;
        QList<QJsonObject> profiles;
        for (const auto& item : data->availableProfiles) {
            MinecraftProfile parsed;
            if (Yggdrasil::profile(item.toObject(), parsed)) {
                names.append(parsed.name + " (" + parsed.id + ")");
                profiles.append(item.toObject());
            }
        }
        if (profiles.isEmpty()) {
            QMessageBox::warning(parent, QObject::tr("无法登录"), QObject::tr("没有可用角色，请先在皮肤站创建角色。"));
            return nullptr;
        }
        bool ok = false;
        const auto selected =
            QInputDialog::getItem(parent, QObject::tr("选择角色"), QObject::tr("使用哪个皮肤站角色？"), names, 0, false, &ok);
        if (!ok)
            return nullptr;
        Yggdrasil::profile(profiles.at(names.indexOf(selected)), data->minecraftProfile);
    }
    // Bind the chosen character to this token, including single-profile responses.
    auto refresh = account->refresh();
    ProgressDialog binding(parent);
    binding.execWithTask(refresh.get());
    return refresh->wasSuccessful() && account->hasProfile() ? account : nullptr;
}
