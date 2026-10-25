// SPDX-License-Identifier: GPL-3.0-only
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "net/ChecksumValidator.h"
#include "net/Request.h"

namespace {
class DownloadServer : public QTcpServer {
   public:
    QByteArray data;
    QString mode;
    int ranges = 0;
    int fullRequests = 0;
    int activeRanges = 0;
    int peakRanges = 0;
    QStringList requestedRanges;

    explicit DownloadServer(QString behavior) : mode(std::move(behavior))
    {
        // An uneven length and nonuniform contents catch ordering/boundary bugs.
        data.resize(mode == "small" ? 12345 : 8 * 1024 * 1024 + 137);
        for (qsizetype i = 0; i < data.size(); ++i) {
            data[i] = static_cast<char>((i * 31 + i / 251) % 256);
        }
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n") || socket->property("handled").toBool()) {
                        return;
                    }
                    socket->setProperty("handled", true);
                    respond(socket, request);
                });
            }
        });
    }

    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/file").arg(serverPort())); }

   private:
    void respond(QTcpSocket* socket, const QByteArray& request)
    {
        if (mode == "redirect" && request.startsWith("GET /file ")) {
            socket->write("HTTP/1.1 302 Found\r\nLocation: " + url().toEncoded() + "-final\r\nContent-Length: 0\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        const auto match = QRegularExpression("(?i)\\r\\nRange: bytes=(\\d+)-(\\d+)").match(QString::fromLatin1(request));
        const bool range = match.hasMatch();
        qint64 start = 0;
        qint64 end = data.size() - 1;
        if (range) {
            ++ranges;
            ++activeRanges;
            peakRanges = qMax(peakRanges, activeRanges);
            connect(socket, &QTcpSocket::disconnected, this, [this] { --activeRanges; });
            requestedRanges.append(match.captured(1) + '-' + match.captured(2));
            start = match.captured(1).toLongLong();
            end = match.captured(2).toLongLong();
        } else {
            ++fullRequests;
        }
        const bool partial = range && mode != "ignore";
        auto body = partial ? data.mid(start, end - start + 1) : data;
        auto headers = QByteArray("HTTP/1.1 ") + (partial ? "206 Partial Content" : "200 OK") + "\r\n";
        headers += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        if (mode != "no-range") {
            headers += "Accept-Ranges: bytes\r\n";
        }
        if (mode != "no-etag") {
            headers += QByteArray("ETag: ") +
                       (mode == "weak-etag"               ? "W/\"version1\""
                        : range && mode == "changed-etag" ? "\"version2\""
                                                          : "\"version1\"") +
                       "\r\n";
        }
        if (partial) {
            headers += "Content-Range: bytes " + QByteArray::number(start + (mode == "bad-range" ? 1 : 0)) + '-' + QByteArray::number(end) +
                       '/' + QByteArray::number(data.size()) + "\r\n";
        }
        if (range && mode == "corrupt") {
            body[0] = static_cast<char>(body[0] ^ 0x7f);
        }
        if (range && mode == "truncate") {
            body.chop(19);
        }
        socket->write(headers + "Connection: close\r\n\r\n");
        // Delay bodies so all range connections can be observed concurrently.
        QTimer::singleShot(mode == "cancel" ? 500 : 40, socket, [socket, body] {
            socket->write(body);
            socket->disconnectFromHost();
        });
    }
};
}  // namespace

class RequestDownloadTest : public QObject {
    Q_OBJECT
   private slots:
    void download_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto* mode : { "ranges", "small", "no-range", "no-etag", "weak-etag", "ignore", "bad-range", "changed-etag", "truncate",
                                  "corrupt", "redirect" }) {
            QTest::newRow(mode) << QString(mode);
        }
    }

    void download()
    {
        QFETCH(QString, mode);
        DownloadServer server(mode);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("download.jar");
        QNetworkAccessManager network;
        network.setProxy(QNetworkProxy::NoProxy);
        auto request = Net::Request::makeFile(server.url(), path);
        const auto cleanup = qScopeGuard([&] {
            request.reset();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        });
        request->setNetwork(&network);
        request->addValidator(
            new Net::ChecksumValidator(QCryptographicHash::Sha256, QCryptographicHash::hash(server.data, QCryptographicHash::Sha256)));
        QSignalSpy finished(request.get(), &Task::finished);
        request->start();
        QVERIFY(finished.wait(15000));
        QCOMPARE(finished.count(), 1);
        if (mode == "corrupt") {
            QVERIFY(!request->wasSuccessful());
            QVERIFY(!QFile::exists(path));
            // NetJob retries the same Request object after a failure.
            server.mode = "ranges";
            finished.clear();
            request->start();
            QVERIFY(finished.wait(15000));
            QVERIFY2(request->wasSuccessful(), qPrintable(request->failReason()));
            QFile retried(path);
            QVERIFY(retried.open(QIODevice::ReadOnly));
            QCOMPARE(retried.readAll(), server.data);
            return;
        }
        QVERIFY2(request->wasSuccessful(), qPrintable(request->failReason()));
        QCOMPARE(request->error(), QNetworkReply::NoError);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), server.data);
        if (mode == "small" || mode == "no-range" || mode == "no-etag" || mode == "weak-etag") {
            QCOMPARE(server.ranges, 0);
            QCOMPARE(server.fullRequests, 1);
        } else {
            QCOMPARE(server.ranges, 4);
            QCOMPARE(server.peakRanges, 4);
            QCOMPARE(server.requestedRanges.size(), 4);
            for (int i = 0; i < 4; ++i) {
                QVERIFY(server.requestedRanges.contains(
                    QString("%1-%2").arg(server.data.size() * i / 4).arg(server.data.size() * (i + 1) / 4 - 1)));
            }
            QCOMPARE(server.fullRequests, mode == "ranges" || mode == "redirect" ? 1 : 2);
        }
    }

    void cancel()
    {
        DownloadServer server("cancel");
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir;
        const auto path = dir.filePath("download.jar");
        // An interrupted update must preserve the previously installed file.
        QFile previous(path);
        QVERIFY(previous.open(QIODevice::WriteOnly));
        previous.write("old file");
        previous.close();
        QNetworkAccessManager network;
        network.setProxy(QNetworkProxy::NoProxy);
        auto request = Net::Request::makeFile(server.url(), path);
        const auto cleanup = qScopeGuard([&] {
            request.reset();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        });
        request->setNetwork(&network);
        QSignalSpy finished(request.get(), &Task::finished);
        QSignalSpy aborted(request.get(), &Task::aborted);
        request->start();
        QTRY_COMPARE(server.ranges, 4);
        QVERIFY(request->abort());
        QCOMPARE(aborted.count(), 1);
        QCOMPARE(finished.count(), 1);
        QTRY_COMPARE(server.activeRanges, 0);
        QVERIFY(previous.open(QIODevice::ReadOnly));
        QCOMPARE(previous.readAll(), QByteArray("old file"));
    }
};

QTEST_GUILESS_MAIN(RequestDownloadTest)
#include "RequestDownload_test.moc"
