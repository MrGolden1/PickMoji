#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// Checks GitHub Releases for a newer PickMoji and, on request, downloads and
// swaps in the new single-file executable. This is the app's only network
// access: it runs only when the user allows it (a tray toggle) or asks for it,
// talks to nothing but api.github.com / the release asset host, and verifies
// the download's SHA-256 before replacing the running binary.
class UpdateChecker final : public QObject {
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // userInitiated: surface "you're up to date" and errors to the user. A
    // silent background check stays quiet unless there is genuinely an update.
    void checkForUpdates(bool userInitiated);
    bool isBusy() const { return m_busy; }
    QString latestVersion() const { return m_latestVersion; }

    // Download the pending update, verify it, swap it in and relaunch. Only
    // meaningful after updateAvailable() has fired.
    void downloadAndApply();

    // Remove the "<exe>.old" left behind by a previous self-update, if any.
    static void cleanupAfterUpdate();

signals:
    void updateAvailable(const QString &version);
    void upToDate();                          // userInitiated checks only
    void checkFailed(const QString &reason);  // userInitiated checks only
    void updateFailed(const QString &reason);
    // Fired after the new binary is in place, just before relaunching, so the
    // owner can release singleton locks the new process needs.
    void aboutToRelaunch();

private:
    void onReleaseReply(QNetworkReply *reply, bool userInitiated);
    void onDownloadReply(QNetworkReply *reply);
    bool swapInExecutable(const QString &downloadedExe, QString &error);

    QNetworkAccessManager *m_net = nullptr;
    bool m_busy = false;
    QString m_latestVersion;
    QString m_downloadUrl;
    QString m_expectedSha256; // lowercase hex; empty when the release omits it
};
