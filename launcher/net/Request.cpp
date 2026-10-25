// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (c) 2022 flowln <flowlnlnln@gmail.com>
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *  Copyright (C) 2023 TheKodeToad <TheKodeToad@proton.me>
 *  Copyright (C) 2023 Rachel Powers <508861+Ryex@users.noreply.github.com>
 *  Copyright (c) 2023 Trial97 <alexandru.tripon97@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "Request.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QIODevice>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include "BuildConfig.h"

#ifdef LAUNCHER_APPLICATION
#include "Application.h"
#include "net/ApiHeaderProxy.h"
#include "net/ChecksumValidator.h"
#include "net/MetaCacheSink.h"
#include "settings/SettingsObject.h"
#endif
#include "net/ByteArraySink.h"
#include "net/FileSink.h"
#include "net/Logging.h"
#include "tasks/Task.h"

#include "MMCTime.h"
#include "StringUtils.h"

namespace Net {

namespace {
auto logCatForMethod(HttpMethod method) -> Request::LogCatFunc
{
    switch (method.value()) {
        case HttpMethod::Get:
            return taskDownloadLogC;
        case HttpMethod::Post:
            return taskUploadLogC;
        default:
            break;
    }
    return taskNetLogC;
}
}  // namespace

Request::Request() : Request(Spec{}) {}

Request::Request(const QUrl& url, Options options, const QString& name)
    : Request(Spec{ .method = HttpMethod::Get, .url = url, .data = std::monostate{}, .options = options, .name = name })
{}

Request::Request(const QUrl& url, QByteArray postData, Options options)
    : Request(Spec{ .method = HttpMethod::Post, .url = url, .data = std::move(postData), .options = options })
{}

Request::Request(const Spec& spec) : m_options(spec.options), m_url(spec.url), m_httpMethod(spec.method), m_postData(spec.data)
{
    connect(&m_retryTimer, &QTimer::timeout, this, &Request::executeTask);

    if (spec.name.isEmpty()) {
        setObjectName(QString("BYTES:") + m_url.toString());
    } else {
        setObjectName(spec.name);
    }
    m_logCat = logCatForMethod(m_httpMethod);
#ifdef LAUNCHER_APPLICATION
    if (spec.options.testFlag(Option::AddAPIHeaders)) {
        addHeaderProxy(std::make_unique<ApiHeaderProxy>());
    }
#endif
}

void Request::addValidator(Validator* v)
{
    m_sink->addValidator(v);
}

void Request::executeTask()
{
    stopSegments();
    m_segments.clear();
    setStatus(tr("Requesting %1").arg(StringUtils::truncateUrlHumanFriendly(m_url, 80)));

    if (m_network == nullptr) {
#ifdef LAUNCHER_APPLICATION
        m_network = APPLICATION->network();
#else
        qCCritical(m_logCat) << getUid().toString() << "No network manager set for request:" << m_url.toString();
        emitFailed("No network manager set for request");
        return;
#endif
    }
    if (getState() == Task::State::AbortedByUser) {
        qCWarning(m_logCat) << getUid().toString() << "Attempt to start an aborted Request:" << m_url.toString();
        emitAborted();
        return;
    }

    QNetworkRequest request(m_url);
    auto result = m_sink->init(request);
    if (!result) {
        emitFailed(result.error());
        return;
    }
    switch (*result) {
        case Sink::InitType::Ok:
            qCDebug(m_logCat) << getUid().toString() << "Running" << m_url.toString();
            break;
        case Sink::InitType::CacheHit:
            qCDebug(m_logCat) << getUid().toString() << "Request cache hit" << m_url.toString();
            emitSucceeded();
            return;
    }

    auto userAgent = BuildConfig.USER_AGENT;
#ifdef LAUNCHER_APPLICATION
    if (auto* app = APPLICATION_DYN) {
        userAgent = app->getUserAgent();
    }
#endif
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent.toUtf8());
    for (auto& headerProxy : m_headerProxies) {
        headerProxy->writeHeaders(request);
    }

    request.setTransferTimeout();
#ifdef LAUNCHER_APPLICATION
    if (auto* app = APPLICATION_DYN) {
        request.setTransferTimeout(app->settings()->get("RequestTimeout").toInt() * 1000);
    }
#endif

    m_lastProgressTime = std::chrono::steady_clock::now();
    m_lastProgressBytes = 0;
    m_bodyStarted = false;

    auto* rep = getReply(request);
    if (rep == nullptr) {  // it failed
        return;
    }
    m_reply.reset(rep);
    connect(rep, &QNetworkReply::uploadProgress, this, &Request::onProgress);
    connect(rep, &QNetworkReply::downloadProgress, this, &Request::onProgress);
    connect(rep, &QNetworkReply::finished, this, &Request::downloadFinished);
    connect(rep, &QNetworkReply::errorOccurred, this, &Request::downloadError);
    connect(rep, &QNetworkReply::sslErrors, this, &Request::sslErrors);
    connect(rep, &QNetworkReply::readyRead, this, &Request::downloadReadyRead);
    connect(rep, &QNetworkReply::metaDataChanged, this, [this, request] { tryStartSegments(request); });
}

void Request::tryStartSegments(const QNetworkRequest& request)
{
    // Keep small files, API responses, compressed responses and conditional
    // cache validation on the ordinary path. Network I/O is asynchronous: no
    // blocking worker threads or cross-thread QNetworkAccessManager access.
    if (m_segmentFallback || !m_segments.empty() || !isRunning() || m_httpMethod != HttpMethod::Get ||
        dynamic_cast<FileSink*>(m_sink.get()) == nullptr || replyStatusCode() != 200 || request.hasRawHeader("Range") ||
        request.hasRawHeader("If-None-Match") || request.hasRawHeader("If-Modified-Since") || m_bodyStarted ||
        m_reply->hasRawHeader("Content-Encoding")) {
        return;
    }
    const auto total = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    if (total < 8 * 1024 * 1024 || m_reply->rawHeader("Accept-Ranges").toLower() != "bytes") {
        return;
    }
    // A strong validator prevents splicing together different revisions.
    const auto etag = m_reply->rawHeader("ETag");
    if (etag.isEmpty() || etag.startsWith("W/")) {
        return;
    }
    int count = 4;
#ifdef LAUNCHER_APPLICATION
    if (auto* app = APPLICATION_DYN) {
        count = qBound(1, app->settings()->get("NumberOfConcurrentDownloads").toInt(), 4);
    }
#endif
    if (count == 1) {
        return;
    }
    m_segmentTotal = total;
    m_segmentETag = etag;
    for (int i = 0; i < count; ++i) {
        auto segment = std::make_unique<Segment>();
        segment->start = total / count * i + total % count * i / count;
        segment->end = total / count * (i + 1) + total % count * (i + 1) / count - 1;
        if (!segment->file.open()) {
            m_segments.clear();
            return;  // Temporary storage unavailable; continue the original GET.
        }
        m_segments.push_back(std::move(segment));
    }

    // Retain the original response metadata for cache finalization, but stop
    // its body before starting the range requests.
    disconnect(m_reply.get(), nullptr, this, nullptr);
    m_reply->abort();
    for (size_t i = 0; i < m_segments.size(); ++i) {
        auto& segment = *m_segments[i];
        QNetworkRequest rangeRequest(m_reply->url());
        rangeRequest.setHeader(QNetworkRequest::UserAgentHeader, request.header(QNetworkRequest::UserAgentHeader));
        rangeRequest.setTransferTimeout(request.transferTimeout());
        // Re-evaluate host-specific credentials after redirects.
        for (const auto& proxy : m_headerProxies) {
            proxy->writeHeaders(rangeRequest);
        }
        rangeRequest.setRawHeader("Accept-Encoding", "identity");
        rangeRequest.setRawHeader("Range",
                                  QByteArray("bytes=") + QByteArray::number(segment.start) + '-' + QByteArray::number(segment.end));
        rangeRequest.setRawHeader("If-Range", etag);
        segment.reply.reset(m_network->get(rangeRequest));
        segment.reply->setReadBufferSize(256 * 1024);
        connect(segment.reply.get(), &QNetworkReply::readyRead, this, [this, i] { readSegment(i); });
        connect(segment.reply.get(), &QNetworkReply::finished, this, [this, i] { finishSegment(i); });
        connect(segment.reply.get(), &QNetworkReply::sslErrors, this, &Request::sslErrors);
    }
}

void Request::stopSegments()
{
    for (auto& segment : m_segments) {
        if (segment->reply) {
            disconnect(segment->reply.get(), nullptr, this, nullptr);
            if (!segment->reply->isFinished()) {
                segment->reply->abort();
            }
        }
    }
}

void Request::fallbackFromSegments()
{
    stopSegments();
    m_segmentFallback = true;
    // Defer deletion of replies until their signal handlers have returned.
    QTimer::singleShot(0, this, [this] {
        m_segments.clear();
        if (isRunning()) {
            m_sink->abort();
            executeTask();
        }
    });
}

void Request::readSegment(size_t index)
{
    if (!isRunning() || m_segmentFallback) {
        return;
    }
    auto& segment = *m_segments[index];
    auto& reply = *segment.reply;
    const auto expectedRange = QByteArray("bytes ") + QByteArray::number(segment.start) + '-' + QByteArray::number(segment.end) + '/' +
                               QByteArray::number(m_segmentTotal);
    if (reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206 || reply.rawHeader("Content-Range") != expectedRange ||
        reply.rawHeader("ETag") != m_segmentETag || reply.hasRawHeader("Content-Encoding")) {
        fallbackFromSegments();
        return;
    }
    const auto data = reply.readAll();
    if (segment.file.size() + data.size() > segment.end - segment.start + 1) {
        fallbackFromSegments();
        return;
    }
    if (segment.file.write(data) != data.size()) {
        stopSegments();
        m_sink->abort();
        emitFailed(tr("Failed to write download segment: %1").arg(segment.file.errorString()));
        return;
    }
    qint64 received = 0;
    for (const auto& part : m_segments) {
        received += part->file.size();
    }
    onProgress(received, m_segmentTotal);
}

void Request::finishSegment(size_t index)
{
    if (!isRunning() || m_segmentFallback) {
        return;
    }
    auto& segment = *m_segments[index];
    if (segment.reply->error() != QNetworkReply::NoError) {
        fallbackFromSegments();
        return;
    }
    readSegment(index);
    if (!isRunning() || m_segmentFallback) {
        return;
    }
    if (segment.file.size() != segment.end - segment.start + 1) {
        fallbackFromSegments();
        return;
    }
    segment.finished = true;
    if (!std::all_of(m_segments.begin(), m_segments.end(), [](const auto& part) { return part->finished; })) {
        return;
    }
    // Feed the assembled file through the existing validators and atomic save.
    for (auto& part : m_segments) {
        if (!part->file.seek(0)) {
            fallbackFromSegments();
            return;
        }
        while (!part->file.atEnd()) {
            const auto data = part->file.read(256 * 1024);
            if (data.isEmpty()) {
                fallbackFromSegments();
                return;
            }
            if (auto result = m_sink->write(data); !result) {
                m_sink->abort();
                emitFailed(result.error());
                return;
            }
        }
        part->file.remove();
    }
    if (auto result = m_sink->finalize(*m_reply); !result) {
        m_sink->abort();
        emitFailed(result.error());
        return;
    }
    // Expose the successful range response through error()/replyStatusCode(),
    // rather than the canceled original GET used for cache metadata above.
    m_reply = std::move(m_segments.back()->reply);
    disconnect(m_reply.get(), nullptr, this, nullptr);
    emitSucceeded();
}

void Request::onProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - m_lastProgressTime;

    // use milliseconds for speed precision
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    auto bytesReceivedSince = bytesReceived - m_lastProgressBytes;
    auto dlSpeedBps = static_cast<double>(bytesReceivedSince) / static_cast<double>(elapsedMs.count()) * 1000;
    auto remainingTimeS = static_cast<double>(bytesTotal - bytesReceived) / dlSpeedBps;

    //: Current amount of bytes downloaded, out of the total amount of bytes in the download
    QString dlProgress = tr("%1 / %2")
                             .arg(StringUtils::humanReadableFileSize(static_cast<double>(bytesReceived)))
                             .arg(StringUtils::humanReadableFileSize(static_cast<double>(bytesTotal)));

    QString dlSpeedStr;
    if (elapsedMs.count() > 0) {
        auto strEta = bytesTotal > 0 ? Time::humanReadableDuration(remainingTimeS) : tr("unknown");
        //: Download speed, in bytes per second (remaining download time in parenthesis)
        dlSpeedStr = tr("%1 /s (%2)").arg(StringUtils::humanReadableFileSize(dlSpeedBps)).arg(strEta);
    } else {
        //: Download speed at 0 bytes per second
        dlSpeedStr = tr("0 B/s");
    }

    setDetails(dlProgress + "\n" + dlSpeedStr);

    setProgress(bytesReceived, bytesTotal);
}

void Request::downloadError(QNetworkReply::NetworkError error)
{
    if (error == QNetworkReply::OperationCanceledError) {
        qCCritical(m_logCat) << getUid().toString() << "Aborted" << m_url.toString();
        m_state = State::Failed;
    } else if (replyStatusCode() == 429 /* HTTP Too Many Requests*/ && m_options.testFlag(Option::AutoRetry)) {
        qCDebug(m_logCat) << getUid().toString() << "Rate Limited!";
        auto delay = static_cast<int64_t>(10 * std::pow(2, m_retryCount));
        if (m_reply->hasRawHeader("Retry-After")) {
            auto retryAfter = m_reply->rawHeader("Retry-After");
            if (retryAfter.trimmed().endsWith("GMT")) /* HTTP Date format */ {
                auto afterTimestamp = QDateTime::fromString(QString::fromUtf8(retryAfter.trimmed()), "ddd, dd MMM yyyy HH:mm:ss 'GMT'");
                if (afterTimestamp.isValid()) {
                    auto seconds = QDateTime::currentDateTime().secsTo(afterTimestamp);
                    if (seconds > 0) {
                        delay = seconds;
                    }
                }
            } else {
                bool ok = false;
                auto seconds = retryAfter.toLongLong(&ok);
                if (ok && seconds > 0) {
                    delay = seconds;
                }
            }
        }
        handleAutoRetry(delay);
    } else {
        if (m_options.testFlag(Option::AcceptLocalFiles)) {
            if (m_sink->hasLocalData()) {
                m_state = State::Succeeded;
                return;
            }
        }
        // error happened during download.
        qCCritical(m_logCat) << getUid().toString() << "Failed" << m_url.toString() << "with error" << error;
        if (m_reply) {
            qCCritical(m_logCat) << getUid().toString() << "HTTP status:" << replyStatusCode() << errorString();
        }
        if (m_errorResponse.size() > 0) {
            qCCritical(m_logCat) << getUid().toString() << "Response from server:" << m_errorResponse;
        }
        m_state = State::Failed;
    }
}

void Request::sslErrors(const QList<QSslError>& errors)
{
    int i = 1;
    for (const auto& error : errors) {
        qCCritical(m_logCat).nospace() << getUid().toString() << " Request " << m_url.toString() << " SSL Error #" << i << ": "
                                       << error.errorString();
        auto cert = error.certificate();
        qCCritical(m_logCat) << getUid().toString() << "Certificate in question:\n" << cert.toText();
        i++;
    }
}

auto Request::handleRedirect() -> bool
{
    QUrl redirect = m_reply->header(QNetworkRequest::LocationHeader).toUrl();
    if (!redirect.isValid()) {
        if (!m_reply->hasRawHeader("Location")) {
            // no redirect -> it's fine to continue
            return false;
        }
        // there is a Location header, but it's not correct. we need to apply some workarounds...
        QByteArray redirectBA = m_reply->rawHeader("Location");
        if (redirectBA.size() == 0) {
            // empty, yet present redirect header? WTF?
            return false;
        }
        QString redirectStr = QString::fromUtf8(redirectBA);

        if (redirectStr.startsWith("//")) {
            /*
             * IF the URL begins with //, we need to insert the URL scheme.
             * See: https://bugreports.qt.io/browse/QTBUG-41061
             * See: http://tools.ietf.org/html/rfc3986#section-4.2
             */
            redirectStr = m_reply->url().scheme() + ":" + redirectStr;
        } else if (redirectStr.startsWith("/")) {
            /*
             * IF the URL begins with /, we need to process it as a relative URL
             */
            auto url = m_reply->url();
            url.setPath(redirectStr, QUrl::TolerantMode);
            redirectStr = url.toString();
        }

        /*
         * Next, make sure the URL is parsed in tolerant mode. Qt doesn't parse the location header in tolerant mode, which causes
         * issues.
         * FIXME: report Qt bug for this
         */
        redirect = QUrl(redirectStr, QUrl::TolerantMode);
        if (!redirect.isValid()) {
            qCWarning(m_logCat) << getUid().toString() << "Failed to parse redirect URL:" << redirectStr;
            downloadError(QNetworkReply::ProtocolFailure);
            return false;
        }
        qCDebug(m_logCat) << getUid().toString() << "Fixed location header:" << redirect;
    } else {
        qCDebug(m_logCat) << getUid().toString() << "Location header:" << redirect;
    }

    m_url = QUrl(redirect.toString());
    qCDebug(m_logCat) << getUid().toString() << "Following redirect to" << m_url.toString();
    if (++m_redirectCount > 10) {
        qCWarning(m_logCat) << getUid().toString() << "Too many redirects for" << m_url.toString();
        m_sink->abort();
        emitFailed(tr("Too many redirects"));
        return true;
    }
    executeTask();

    return true;
}

void Request::handleAutoRetry(int64_t delay)
{
    m_retryCount++;
    if (delay > 60 || m_retryCount > 4) {
        /* 1 minute is too long to wait for retry, fail for now */
        m_state = State::Failed;
        auto retryAfter = QDateTime::currentDateTime().addSecs(delay);
        emitFailed(tr("Request Rate Limited for %n second(s): Retry After %1", "seconds", static_cast<int>(delay))
                       .arg(retryAfter.toLocalTime().toString(QLocale::system().dateTimeFormat(QLocale::ShortFormat))));
        return;
    }
    qCDebug(m_logCat) << getUid().toString() << "Retyring Request in" << delay << "seconds";
    setStatus(tr("Rate Limited: Waiting %n second(s)", "seconds", static_cast<int>(delay)));
    m_retryTimer.setTimerType(Qt::VeryCoarseTimer);
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(static_cast<int>(delay) * 1000);
    m_retryTimer.start();
}

void Request::downloadFinished()
{
    // currently waiting for retry
    if (m_retryTimer.isActive()) {
        return;
    }

    // handle HTTP redirection first
    if (handleRedirect()) {
        qCDebug(m_logCat) << getUid().toString() << "Request redirected:" << m_url.toString();
        return;
    }

    // if the download failed before this point ...
    if (m_state == State::Succeeded)  // pretend to succeed so we continue processing :)
    {
        qCDebug(m_logCat) << getUid().toString() << "Request failed but we are allowed to proceed:" << m_url.toString();
        m_sink->abort();
        emit succeeded();
        emit finished();
        return;
    }
    if (m_state == State::Failed) {
        qCDebug(m_logCat) << getUid().toString() << "Request failed in previous step:" << m_url.toString();
        m_sink->abort();
        m_failReason = m_reply->errorString();
        emit failed(m_reply->errorString());
        emit finished();
        return;
    }
    if (m_state == State::AbortedByUser) {
        qCDebug(m_logCat) << getUid().toString() << "Request aborted in previous step:" << m_url.toString();
        m_sink->abort();
        emit aborted();
        emit finished();
        return;
    }

    // make sure we got all the remaining data, if any
    auto data = m_reply->readAll();
    if (!data.isEmpty()) {
        qCDebug(m_logCat) << getUid().toString() << "Writing extra" << data.size() << "bytes";
        auto result = m_sink->write(data);
        if (!result) {
            qCDebug(m_logCat) << getUid().toString() << "Request failed to write:" << m_url.toString();
            m_sink->abort();
            emitFailed(result.error());
            return;
        }
    }

    // otherwise, finalize the whole graph
    auto result = m_sink->finalize(*m_reply);
    if (!result) {
        qCDebug(m_logCat) << getUid().toString() << "Request failed to finalize:" << m_url.toString();
        m_sink->abort();
        emitFailed(result.error());
        return;
    }

    qCDebug(m_logCat) << getUid().toString() << "Request succeeded:" << m_url.toString();
    emitSucceeded();
}

void Request::downloadReadyRead()
{
    if (m_state == State::Running) {
        auto data = m_reply->readAll();
        // Once any body has reached the sink, switching to ranges would append
        // it twice. This also guards against repeated metadata notifications.
        m_bodyStarted = m_bodyStarted || !data.isEmpty();
        auto result = m_sink->write(data);
        if (replyStatusCode() >= 400) {
            m_errorResponse.append(data);
        }
        if (!result) {
            m_state = Task::State::Failed;
            m_failReason = result.error();
            qCCritical(m_logCat) << getUid().toString() << "Failed to process response chunk:" << m_failReason;
        }
        // qDebug() << "Request" << m_url.toString() << "gained" << data.size() << "bytes";
    } else {
        qCCritical(m_logCat) << getUid().toString() << "Cannot write download data! illegal status" << m_status;
    }
}

auto Request::abort() -> bool
{
    m_retryTimer.stop();
    m_state = State::AbortedByUser;
    if (!m_segments.empty()) {
        stopSegments();
        m_sink->abort();
        emit aborted();
        emit finished();
        return true;
    }
    if (m_reply && !m_reply->isFinished()) {
        disconnect(m_reply.get(), &QNetworkReply::errorOccurred, nullptr, nullptr);
        m_reply->abort();
    } else {
        emit aborted();
        emit finished();
    }
    return true;
}

int Request::replyStatusCode() const
{
    return m_reply ? m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : -1;
}

QNetworkReply::NetworkError Request::error() const
{
    return m_reply ? m_reply->error() : QNetworkReply::NoError;
}

QUrl Request::url() const
{
    return m_url;
}

QString Request::errorString() const
{
    return m_reply ? m_reply->errorString() : "";
}

void Request::enableAutoRetry(bool enable)
{
    if (enable) {
        m_options |= Option::AutoRetry;
    } else {
        m_options &= ~static_cast<std::uint8_t>(Option::AutoRetry);
    }
}

QNetworkReply* Request::getReply(QNetworkRequest& request)
{
    if (m_httpMethod == HttpMethod::Get) {
        Q_ASSERT(std::holds_alternative<std::monostate>(m_postData));
        return m_network->get(request);
    }
    return std::visit(
        [this, &request](const auto& data) -> QNetworkReply* {
            using T = std::remove_cvref_t<decltype(data)>;
            const auto verb = m_httpMethod.toString().toUtf8();
            if constexpr (std::is_same_v<T, QByteArray>) {
                if (m_httpMethod == HttpMethod::Post && !request.hasRawHeader("Content-Type")) {
                    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
                }
                return m_network->sendCustomRequest(request, verb, data);
            } else if constexpr (std::is_same_v<T, ByteArrayFactory>) {
                if (m_httpMethod == HttpMethod::Post && !request.hasRawHeader("Content-Type")) {
                    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
                }
                auto body = data();
                if (!body) {
                    emitFailed(body.error());
                    return nullptr;
                }
                return m_network->sendCustomRequest(request, verb, body.value());

            } else if constexpr (std::is_same_v<T, std::monostate>) {
                return m_network->sendCustomRequest(request, verb);
            } else if constexpr (std::is_same_v<T, DeviceFactory>) {
                auto payload = data();
                if (!payload) {
                    emitFailed(payload.error());
                    return nullptr;
                }
                auto device = payload.value();
                if (device != nullptr) {
                    device->setParent(this);
                }
                return m_network->sendCustomRequest(request, verb, device);
            } else if constexpr (std::is_same_v<T, MultiPartFactory>) {
                auto payload = data();
                if (!payload) {
                    emitFailed(payload.error());
                    return nullptr;
                }
                auto multiPart = payload.value();
                if (multiPart != nullptr) {
                    multiPart->setParent(this);
                }
                return m_network->sendCustomRequest(request, verb, multiPart);
            }
        },
        m_postData);
}

#ifdef LAUNCHER_APPLICATION
auto Request::makeCached(const QUrl& url, MetaEntryPtr entry, Options options) -> Ptr
{
    auto dl = Ptr(new Request(url, options, (QString("CACHE:") + url.toString())));
    auto* md5Node = new ChecksumValidator(QCryptographicHash::Md5);
    auto* cachedNode = new MetaCacheSink(std::move(entry), md5Node, options.testFlag(Option::MakeEternal));
    dl->m_sink.reset(cachedNode);
    return dl;
}
#endif

auto Request::makeByteArray(const QUrl& url, QByteArray postData, Options options) -> std::pair<Ptr, QByteArray*>
{
    auto dl = Ptr(new Request(url, std::move(postData), options));

    auto sink = std::make_unique<ByteArraySink>();
    auto* response = sink->output();
    dl->m_sink = std::move(sink);

    return { dl, response };
}

auto Request::makeByteArray(const QUrl& url, Options options) -> std::pair<Ptr, QByteArray*>
{
    auto dl = Ptr(new Request(url, options));

    auto sink = std::make_unique<ByteArraySink>();
    auto* response = sink->output();
    dl->m_sink = std::move(sink);

    return { dl, response };
}

auto Request::makeFile(const QUrl& url, const QString& path, Options options) -> Ptr
{
    auto dl = Ptr(new Request(url, options, QString("FILE:") + url.toString()));
    dl->m_sink = std::make_unique<FileSink>(path);

    return dl;
}

auto Request::makeCustomRequest(const Spec& spec) -> Ptr
{
    return Ptr(new Request(spec));
}

}  // namespace Net
