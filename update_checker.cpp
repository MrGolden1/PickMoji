#include "update_checker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QString>
#include <QUrl>
#include <QVersionNumber>

#ifndef PICKMOJI_VERSION
#define PICKMOJI_VERSION "0.0.0"
#endif

namespace {
// The one endpoint this app ever contacts (plus wherever GitHub redirects the
// asset download to). Kept as a constant so the network surface is obvious.
constexpr auto RELEASES_LATEST_URL =
    "https://api.github.com/repos/MrGolden1/PickMoji/releases/latest";

QString userAgent() {
    // GitHub's API rejects requests without a User-Agent.
    return QStringLiteral("PickMoji/") + QCoreApplication::applicationVersion();
}

QString oldExecutablePath() {
    return QCoreApplication::applicationFilePath() + QStringLiteral(".old");
}
}

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

void UpdateChecker::checkForUpdates(bool userInitiated) {
    if (m_busy)
        return;
    m_busy = true;

    QNetworkRequest request{QUrl(QString::fromLatin1(RELEASES_LATEST_URL))};
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, userInitiated]() {
        onReleaseReply(reply, userInitiated);
    });
}

void UpdateChecker::onReleaseReply(QNetworkReply *reply, bool userInitiated) {
    reply->deleteLater();
    m_busy = false;

    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        // 404 simply means no release has been published yet.
        if (http == 404) {
            if (userInitiated)
                emit upToDate();
            return;
        }
        if (userInitiated)
            emit checkFailed(reply->errorString());
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
    QString tag = root.value(QStringLiteral("tag_name")).toString();
    while (!tag.isEmpty() && !tag.at(0).isDigit()) // drop a leading "v"
        tag.remove(0, 1);

    const QVersionNumber latest = QVersionNumber::fromString(tag);
    const QVersionNumber current = QVersionNumber::fromString(QStringLiteral(PICKMOJI_VERSION));
    if (latest.isNull() || latest <= current) {
        if (userInitiated)
            emit upToDate();
        return;
    }

    // Locate the .exe asset and, if present, its SHA-256 digest.
    m_downloadUrl.clear();
    m_expectedSha256.clear();
    const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        if (!asset.value(QStringLiteral("name")).toString().endsWith(QStringLiteral(".exe"),
                                                                     Qt::CaseInsensitive))
            continue;
        m_downloadUrl = asset.value(QStringLiteral("browser_download_url")).toString();
        const QString digest = asset.value(QStringLiteral("digest")).toString();
        if (digest.startsWith(QStringLiteral("sha256:")))
            m_expectedSha256 = digest.mid(7).toLower();
        break;
    }

    if (m_downloadUrl.isEmpty()) {
        if (userInitiated)
            emit checkFailed(QStringLiteral("The latest release has no downloadable .exe."));
        return;
    }

    m_latestVersion = latest.toString();
    emit updateAvailable(m_latestVersion);
}

void UpdateChecker::downloadAndApply() {
    if (m_busy || m_downloadUrl.isEmpty())
        return;
    m_busy = true;

    QNetworkRequest request{QUrl(m_downloadUrl)};
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    // browser_download_url redirects to the asset host; follow it.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onDownloadReply(reply); });
}

void UpdateChecker::onDownloadReply(QNetworkReply *reply) {
    reply->deleteLater();
    m_busy = false;

    if (reply->error() != QNetworkReply::NoError) {
        emit updateFailed(reply->errorString());
        return;
    }

    const QByteArray payload = reply->readAll();
    if (!m_expectedSha256.isEmpty()) {
        const QString actual = QString::fromLatin1(
            QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        if (actual.compare(m_expectedSha256, Qt::CaseInsensitive) != 0) {
            emit updateFailed(QStringLiteral(
                "The download's checksum did not match; it may be corrupt. Update aborted."));
            return;
        }
    }

    // Write beside the current exe (same volume, so the rename below is atomic).
    const QString downloaded =
        QCoreApplication::applicationDirPath() + QStringLiteral("/PickMoji.update-download.exe");
    QFile::remove(downloaded);
    QFile file(downloaded);
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size()) {
        file.close();
        QFile::remove(downloaded);
        emit updateFailed(QStringLiteral("Could not save the downloaded update."));
        return;
    }
    file.close();

    QString error;
    if (!swapInExecutable(downloaded, error)) {
        QFile::remove(downloaded);
        emit updateFailed(error);
        return;
    }

    // The exe at applicationFilePath() is the new build now. Hand the singleton
    // lock back (so the child can claim it), start the new build, and quit.
    emit aboutToRelaunch();
    QProcess::startDetached(QCoreApplication::applicationFilePath(),
                            {QStringLiteral("--background")});
    QCoreApplication::quit();
}

bool UpdateChecker::swapInExecutable(const QString &downloadedExe, QString &error) {
    const QString exePath = QCoreApplication::applicationFilePath();
    const QString oldPath = oldExecutablePath();

    QFile::remove(oldPath); // clear any leftover from a previous update

    // A running exe cannot be deleted, but it can be renamed. Move it aside,
    // then move the new one into its place.
    if (!QFile::rename(exePath, oldPath)) {
        error = QStringLiteral("Could not replace the running program. If PickMoji is installed "
                               "in a protected folder, download the update manually.");
        return false;
    }
    if (!QFile::rename(downloadedExe, exePath)) {
        QFile::rename(oldPath, exePath); // roll back so we are never left with no exe
        error = QStringLiteral("Could not put the new version in place. Update aborted.");
        return false;
    }
    return true;
}

void UpdateChecker::cleanupAfterUpdate() {
    // The just-replaced binary from a prior update. The previous process may
    // still be exiting, so removal can fail once; that is fine — the next launch
    // clears it.
    const QString oldPath = oldExecutablePath();
    if (QFile::exists(oldPath))
        QFile::remove(oldPath);
    QFile::remove(QCoreApplication::applicationDirPath()
                  + QStringLiteral("/PickMoji.update-download.exe"));
}
