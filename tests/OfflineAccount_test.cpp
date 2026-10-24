#include <QTest>

#include "LaunchController.h"

#include "minecraft/auth/AccountList.h"
#include "minecraft/auth/AuthFlow.h"
#include "minecraft/auth/AuthSession.h"
#include "minecraft/auth/MinecraftAccount.h"

class OfflineAccountTest : public QObject {
    Q_OBJECT
   private slots:
    void selectsOfflineWithoutMicrosoft()
    {
        LaunchController controller;
        controller.setAccountToUse(MinecraftAccount::createOffline("LocalPlayer"));
        controller.setLaunchMode(LaunchMode::Normal);
        QCOMPARE(controller.decideLaunchMode(), LaunchDecision::Continue);
        QCOMPARE(controller.m_actualLaunchMode, LaunchMode::Normal);
        controller.setLaunchMode(LaunchMode::Offline);
        QCOMPARE(controller.decideLaunchMode(), LaunchDecision::Continue);
        QCOMPARE(controller.m_actualLaunchMode, LaunchMode::Offline);

        controller.setLaunchMode(LaunchMode::Demo);
        QCOMPARE(controller.decideLaunchMode(), LaunchDecision::Continue);
        QCOMPARE(controller.m_actualLaunchMode, LaunchMode::Demo);
    }

    void standaloneAccount()
    {
        AccountList accounts;
        QVERIFY(!accounts.anyAccountIsValid());
        const auto account = MinecraftAccount::createOffline("LocalPlayer");
        auto login = account->login();
        login->start();
        QVERIFY(login->wasSuccessful());
        QVERIFY(!account->ownsMinecraft());
        accounts.addAccount(account);
        accounts.setDefaultAccount(account);
        QVERIFY(accounts.anyAccountIsValid());
        QCOMPARE(accounts.defaultAccount(), account);
    }

    void persistenceAndSession()
    {
        auto original = MinecraftAccount::createOffline("LocalPlayer");
        auto login = original->login();
        login->start();
        const auto loaded = MinecraftAccount::loadFromJsonV3(original->saveToJson());
        QVERIFY(loaded);
        QCOMPARE(loaded->accountType(), AccountType::Offline);
        QVERIFY(!loaded->ownsMinecraft());
        auto session = std::make_shared<AuthSession>();
        session->launchMode = LaunchMode::Offline;
        loaded->fillSession(session);
        QCOMPARE(session->launchMode, LaunchMode::Offline);
        QCOMPARE(session->player_name, QString("LocalPlayer"));
        QCOMPARE(session->uuid, MinecraftAccount::uuidFromUsername("LocalPlayer").toString(QUuid::Id128));
        QCOMPARE(session->access_token, QString("0"));
        QCOMPARE(session->user_type, QString("offline"));
    }

    void emptyMicrosoftAccountDoesNotQualify()
    {
        AccountList accounts;
        accounts.addAccount(MinecraftAccount::createBlankMSA());
        QVERIFY(!accounts.anyAccountIsValid());
    }
};

QTEST_GUILESS_MAIN(OfflineAccountTest)
#include "OfflineAccount_test.moc"
