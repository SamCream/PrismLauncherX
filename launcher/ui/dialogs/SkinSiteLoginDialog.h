// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "minecraft/auth/MinecraftAccount.h"
class QWidget;
namespace SkinSiteLoginDialog {
MinecraftAccountPtr newAccount(QWidget* parent,
                               const QString& server = "https://littleskin.cn/api/yggdrasil/",
                               const QString& username = {});
}
