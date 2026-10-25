// SPDX-License-Identifier: GPL-3.0-only
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>
#include <QUrl>

#include "modplatform/flame/FlameModIndex.h"

class FlameDownloadUrlTest : public QObject {
    Q_OBJECT
   private slots:
    void resolve_data()
    {
        QTest::addColumn<int>("id");
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("provided");
        QTest::addColumn<QString>("expected");
        QTest::newRow("null URL") << 1234005 << "mod.jar" << "" << "https://edge.forgecdn.net/files/1234/5/mod.jar";
        QTest::newRow("exact thousand") << 1234000 << "mod.jar" << "" << "https://edge.forgecdn.net/files/1234/0/mod.jar";
        QTest::newRow("escaped filename") << 1234567 << "mod + #?%中文.jar" << ""
                                          << "https://edge.forgecdn.net/files/1234/567/mod%20%2B%20%23%3F%25%E4%B8%AD%E6%96%87.jar";
        QTest::newRow("provided URL") << 1234005 << "mod.jar" << "https://example.com/mod.jar?token=test"
                                      << "https://example.com/mod.jar?token=test";
        QTest::newRow("invalid ID") << 0 << "mod.jar" << "" << "";
        QTest::newRow("empty filename") << 1234005 << "" << "" << "";
    }

    void resolve()
    {
        QFETCH(int, id);
        QFETCH(QString, name);
        QFETCH(QString, provided);
        QFETCH(QString, expected);
        QJsonObject obj{ { "gameVersions", QJsonArray{ "1.21.1", "Fabric" } },
                         { "modId", 123 },
                         { "id", id },
                         { "fileDate", "2026-01-01T00:00:00Z" },
                         { "displayName", "test" },
                         { "fileName", name },
                         { "downloadUrl", provided.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(provided) },
                         { "releaseType", 1 },
                         { "hashes", QJsonArray{ QJsonObject{ { "algo", 1 }, { "value", "abcdef" } } } } };
        const auto result = FlameMod::loadIndexedPackVersion(obj);
        QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error()));
        QCOMPARE(result->downloadUrl, expected);
        QCOMPARE(result->hash, QString("abcdef"));
        QCOMPARE(result->hashType, QString("sha1"));
        if (!expected.isEmpty()) {
            QCOMPARE(QUrl(result->downloadUrl).toEncoded(), expected.toUtf8());
        }
    }
};

QTEST_GUILESS_MAIN(FlameDownloadUrlTest)
#include "FlameDownloadUrl_test.moc"
