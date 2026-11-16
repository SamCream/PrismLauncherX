// SPDX-License-Identifier: GPL-3.0-only
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include "minecraft/GameDirectory.h"
#include "minecraft/auth/DownloadAuthlibInjector.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "minecraft/auth/Yggdrasil.h"
#include "minecraft/auth/steps/YggdrasilStep.h"
#include "net/ChecksumValidator.h"
#include "net/DownloadSource.h"
#include "net/Request.h"

class ProtocolServer : public QTcpServer {
   public:
    std::function<QPair<int, QByteArray>(QByteArray)> handler;
    QList<QByteArray> requests;
    QByteArray extraHeaders;
    ProtocolServer()
    {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto socket = nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    const auto split = bytes->indexOf("\r\n\r\n");
                    if (split < 0)
                        return;
                    const QRegularExpression length("Content-Length: ([0-9]+)", QRegularExpression::CaseInsensitiveOption);
                    const auto match = length.match(QString::fromLatin1(bytes->left(split)));
                    const int size = match.hasMatch() ? match.captured(1).toInt() : 0;
                    if (bytes->size() < split + 4 + size)
                        return;
                    socket->disconnect(this);
                    requests.append(*bytes);
                    const auto [status, body] = handler(*bytes);
                    socket->write("HTTP/1.1 " + QByteArray::number(status) +
                                  " Result\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\n" + extraHeaders + "\r\n" + body);
                    socket->disconnectFromHost();
                    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/").arg(serverPort())); }
};

// Route fixture traffic locally while preserving the URL selected by the production downloader.
class FixtureNetwork : public QNetworkAccessManager {
   public:
    QUrl endpoint;
    QList<QUrl> urls;

   protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request, QIODevice* data = nullptr) override
    {
        urls.append(request.url());
        auto local = request;
        local.setUrl(endpoint.resolved(QUrl(request.url().host() == "bmclapi2.bangbang93.com" ? "mirror" : "official")));
        return QNetworkAccessManager::createRequest(operation, local, data);
    }
};

class LauncherFeaturesTest : public QObject {
    Q_OBJECT
   private slots:
    void directoryModes()
    {
        QTemporaryDir root;
        const auto version = QDir(root.path()).filePath("versions/Test");
        QDir().mkpath(version);
        QCOMPARE(GameDirectory::resolve(version, root.path(), 0, false, {}), root.path());
        QCOMPARE(GameDirectory::resolve(version, root.path(), 1, false, {}), version);
        QCOMPARE(GameDirectory::resolve(version, root.path(), 2, false, {}), root.path());
        QCOMPARE(GameDirectory::resolve(version, root.path(), 2, true, {}), version);
        QCOMPARE(GameDirectory::resolve(version, root.path(), 3, false, "custom"), QDir(version).filePath("custom"));
        QDir().mkpath(QDir(version).filePath(".minecraft"));
        QCOMPARE(GameDirectory::resolve(version, root.path(), 4, false, {}), QDir(version).filePath(".minecraft"));
        QDir().mkpath(QDir(version).filePath("minecraft"));
        QCOMPARE(GameDirectory::resolve(version, root.path(), 4, false, {}), QDir(version).filePath("minecraft"));
    }
    void downloadRouting()
    {
        const QUrl assets("https://resources.download.minecraft.net/ab/abcdef");
        QCOMPARE(Net::DownloadSource::rewrite(assets, "official", {}), assets);
        QCOMPARE(Net::DownloadSource::rewrite(assets, "bmclapi", {}).toString(),
                 QString("https://bmclapi2.bangbang93.com/assets/ab/abcdef"));
        const QUrl library("https://libraries.minecraft.net/com/example/test.jar");
        QCOMPARE(Net::DownloadSource::rewrite(library, "custom", QUrl("https://example.org/mirror/")).toString(),
                 QString("https://example.org/mirror/maven/com/example/test.jar"));
        const QUrl manifest("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json");
        QCOMPARE(Net::DownloadSource::rewrite(manifest, "bmclapi", {}).path(), manifest.path());
        for (const auto& address : { "https://littleskin.cn/api/yggdrasil/authserver/authenticate", "https://login.live.com/token",
                                     "https://api.modrinth.com/v2/project/x" }) {
            const QUrl original(address);
            QCOMPARE(Net::DownloadSource::rewrite(original, "bmclapi", {}), original);
        }
        QVERIFY(!Net::DownloadSource::validBase(QUrl("http://example.org")));
        QVERIFY(!Net::DownloadSource::validBase(QUrl("https://user:password@example.org")));
        QCOMPARE(Net::DownloadSource::rewrite(assets, "custom", QUrl("http://example.org")), assets);
    }
    void mirrorFailureFallsBackWithChecksum()
    {
        ProtocolServer server;
        const QByteArray payload("verified official artifact");
        server.handler = [&](const QByteArray& request) {
            return request.startsWith("GET /mirror") ? qMakePair(503, QByteArray("unavailable")) : qMakePair(200, payload);
        };
        FixtureNetwork network;
        network.endpoint = server.url();
        const QUrl original("https://libraries.minecraft.net/example/test.jar");
        auto [request, response] = Net::Request::makeByteArray(original);
        request->setNetwork(&network);
        request->setDownloadSource("bmclapi", {}, true);
        request->addValidator(
            new Net::ChecksumValidator(QCryptographicHash::Sha256, QCryptographicHash::hash(payload, QCryptographicHash::Sha256)));
        QSignalSpy done(request.get(), &Task::finished);
        request->start();
        QVERIFY(done.wait(5000));
        QVERIFY2(request->wasSuccessful(), qPrintable(request->failReason()));
        QCOMPARE(*response, payload);
        QCOMPARE(network.urls.size(), 2);
        QCOMPARE(network.urls.first().host(), QString("bmclapi2.bangbang93.com"));
        QCOMPARE(network.urls.last(), original);

        auto [disabled, ignored] = Net::Request::makeByteArray(original);
        Q_UNUSED(ignored);
        disabled->setNetwork(&network);
        disabled->setDownloadSource("bmclapi", {}, false);
        QSignalSpy failed(disabled.get(), &Task::finished);
        disabled->start();
        QVERIFY(failed.wait(5000));
        QVERIFY(!disabled->wasSuccessful());
        QCOMPARE(network.urls.size(), 3);
        // shared_qobject_ptr destroys asynchronously. Drain before this fixture's
        // network manager goes out of scope and destroys its child replies.
        request.reset();
        disabled.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void skinSiteAuthenticationAndRefresh()
    {
        ProtocolServer server;
        QNetworkAccessManager network;
        AccountData data;
        data.type = AccountType::Yggdrasil;
        data.authServer = server.url().toString();
        data.authUsername = "user@example.test";
        data.authPassword = "test-password";
        bool refreshed = false;
        const QString id(32, 'a');
        server.handler = [&](const QByteArray& request) {
            const auto body = QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n") + 4)).object();
            if (request.startsWith("POST /authserver/refresh")) {
                refreshed = body.value("selectedProfile").toObject().value("id").toString() == id &&
                            body.value("accessToken").toString() == "token-one" && !body.contains("password");
            }
            QJsonObject reply{ { "clientToken", body.value("clientToken") },
                               { "accessToken", refreshed ? "token-two" : "token-one" },
                               { "selectedProfile", QJsonObject{ { "id", id }, { "name", "SkinPlayer" } } } };
            return qMakePair(200, QJsonDocument(reply).toJson());
        };
        YggdrasilStep login(&data, false, &network);
        QSignalSpy loggedIn(&login, &AuthStep::finished);
        login.perform();
        QVERIFY(loggedIn.wait(5000));
        QCOMPARE(loggedIn.first().at(0).value<AccountTaskState>(), AccountTaskState::STATE_WORKING);
        QVERIFY(data.authPassword.isEmpty());
        QCOMPARE(data.minecraftProfile.id, id);
        QCOMPARE(data.minecraftProfile.name, QString("SkinPlayer"));
        QVERIFY(!data.minecraftEntitlement.ownsMinecraft);
        QVERIFY(!QJsonDocument(data.saveState()).toJson().contains("test-password"));
        AccountData restored;
        QVERIFY(restored.resumeStateFromV3(data.saveState()));
        QCOMPARE(restored.authServer, data.authServer);
        YggdrasilStep refresh(&data, true, &network);
        QSignalSpy refreshedSignal(&refresh, &AuthStep::finished);
        refresh.perform();
        QVERIFY(refreshedSignal.wait(5000));
        QVERIFY(refreshed);
        QCOMPARE(data.accessToken(), QString("token-two"));
    }
    void rejectsInvalidServerAndToken()
    {
        QNetworkAccessManager network;
        AccountData data;
        data.authServer = "http://example.org/api/";
        data.authPassword = "secret";
        YggdrasilStep invalid(&data, false, &network);
        QSignalSpy rejected(&invalid, &AuthStep::finished);
        invalid.perform();
        QCOMPARE(rejected.size(), 1);
        QVERIFY(data.authPassword.isEmpty());
        ProtocolServer server;
        server.handler = [](auto) { return qMakePair(200, QByteArray("{\"accessToken\":\"x\",\"clientToken\":\"wrong\"}")); };
        data.authServer = server.url().toString();
        YggdrasilStep malformed(&data, false, &network);
        QSignalSpy response(&malformed, &AuthStep::finished);
        malformed.perform();
        QVERIFY(response.wait(5000));
        QCOMPARE(response.first().at(0).value<AccountTaskState>(), AccountTaskState::STATE_FAILED_SOFT);
        QVERIFY(data.accessToken().isEmpty());
    }
    void credentialsDoNotFollowRedirects()
    {
        ProtocolServer server;
        server.extraHeaders = "Location: " + server.url().resolved(QUrl("other")).toEncoded() + "\r\n";
        server.handler = [](auto) { return qMakePair(302, QByteArray()); };
        QNetworkAccessManager network;
        AccountData data;
        data.authServer = server.url().toString();
        data.authUsername = "fixture-user";
        data.authPassword = "fixture-secret";
        YggdrasilStep step(&data, false, &network);
        QSignalSpy done(&step, &AuthStep::finished);
        step.perform();
        QVERIFY(done.wait(5000));
        QCOMPARE(done.first().at(0).value<AccountTaskState>(), AccountTaskState::STATE_FAILED_SOFT);
        QCOMPARE(server.requests.size(), 1);
        QVERIFY(data.authPassword.isEmpty());
    }

    void verifiedAgentDownloadAndCache()
    {
        ProtocolServer server;
        QNetworkAccessManager network;
        QTemporaryDir cache;
        const QByteArray jar("PK\x03\x04test-agent-fixture");
        QJsonObject metadata{
            { "download_url", server.url().resolved(QUrl("agent.jar")).toString() },
            { "checksums",
              QJsonObject{ { "sha256", QString::fromLatin1(QCryptographicHash::hash(jar, QCryptographicHash::Sha256).toHex()) } } }
        };
        QVERIFY(DownloadAuthlibInjector::verify(jar, metadata));
        QVERIFY(!DownloadAuthlibInjector::verify(jar + "tampered", metadata));
        server.handler = [&](const QByteArray& request) {
            return qMakePair(200, request.startsWith("GET /agent.jar") ? jar : QJsonDocument(metadata).toJson());
        };
        DownloadAuthlibInjector task(&network, cache.path(), server.url());
        QSignalSpy done(&task, &Task::finished);
        task.start();
        QVERIFY(done.wait(5000));
        QVERIFY2(task.wasSuccessful(), qPrintable(task.failReason()));
        QCOMPARE(server.requests.size(), 2);
        DownloadAuthlibInjector cached(&network, cache.path(), server.url());
        cached.start();
        QVERIFY(cached.wasSuccessful());
        QCOMPARE(server.requests.size(), 2);
        QFile corrupted(QDir(cache.path()).filePath("authlib-injector.jar"));
        QVERIFY(corrupted.open(QIODevice::WriteOnly | QIODevice::Truncate));
        corrupted.write("corrupt");
        corrupted.close();
        DownloadAuthlibInjector repair(&network, cache.path(), server.url());
        QSignalSpy repaired(&repair, &Task::finished);
        repair.start();
        QVERIFY(repaired.wait(5000));
        QVERIFY(repair.wasSuccessful());
        QCOMPARE(server.requests.size(), 4);
    }
};
QTEST_GUILESS_MAIN(LauncherFeaturesTest)
#include "LauncherFeatures_test.moc"
