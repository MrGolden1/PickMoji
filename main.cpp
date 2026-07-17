#include "app_controller.h"
#include "emoji_repository.h"
#include "single_instance.h"
#include "update_checker.h"
#include "usage_store.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QMessageBox>
#include <QPixmapCache>
#include <QStringList>
#include <QTimer>

#ifndef PICKMOJI_VERSION
#define PICKMOJI_VERSION "0.0.0"
#endif

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QPixmapCache::setCacheLimit(4096);
    app.setApplicationName(QStringLiteral("PickMoji"));
    app.setApplicationDisplayName(QStringLiteral("PickMoji"));
    app.setApplicationVersion(QStringLiteral(PICKMOJI_VERSION));
    app.setOrganizationName(QStringLiteral("PickMoji"));
    app.setOrganizationDomain(QStringLiteral("pickmoji.app"));
    app.setFont(QFont(QStringLiteral("Segoe UI"), 10));

    const QStringList arguments = app.arguments();

    // Headless diagnostic: run one update check (exercising HTTPS/TLS in this
    // exact build), record the outcome to %TEMP%, and exit. Used to verify the
    // network path in both the dynamic and the static single-file builds.
    if (arguments.contains(QStringLiteral("--check-update"))) {
        auto *checker = new UpdateChecker(&app);
        const QString logPath = QDir::tempPath() + QStringLiteral("/pickmoji-update-check.log");
        const auto report = [logPath](const QString &line) {
            QFile file(logPath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                file.write(line.toUtf8() + '\n');
            qApp->quit();
        };
        QObject::connect(checker, &UpdateChecker::updateAvailable, qApp,
                         [report](const QString &v) { report(QStringLiteral("AVAILABLE ") + v); });
        QObject::connect(checker, &UpdateChecker::upToDate, qApp,
                         [report]() { report(QStringLiteral("UPTODATE")); });
        QObject::connect(checker, &UpdateChecker::checkFailed, qApp,
                         [report](const QString &r) { report(QStringLiteral("FAILED ") + r); });
        QTimer::singleShot(15000, qApp, [report]() { report(QStringLiteral("TIMEOUT")); });
        checker->checkForUpdates(true);
        return app.exec();
    }

    SingleInstance singleInstance;
    if (!singleInstance.startOrNotifyExisting())
        return 0;

    EmojiRepository repository;
    if (!repository.load()) {
        QMessageBox::critical(nullptr, QStringLiteral("PickMoji"), repository.errorString());
        return 1;
    }

    UsageStore usage;
    AppController controller(&repository, &usage, &singleInstance);
    controller.setDebugAnchor(arguments.contains(QStringLiteral("--debug-anchor")));
    const int previewIndex = arguments.indexOf(QStringLiteral("--render-preview"));
    const bool previewMode = previewIndex >= 0 && previewIndex + 1 < arguments.size();
    controller.start(previewMode || arguments.contains(QStringLiteral("--background")));
    if (previewMode) {
        const int sectionIndex = arguments.indexOf(QStringLiteral("--preview-section"));
        const QString section = sectionIndex >= 0 && sectionIndex + 1 < arguments.size()
            ? arguments.at(sectionIndex + 1) : QString();
        const int delayIndex = arguments.indexOf(QStringLiteral("--preview-delay"));
        bool delayIsValid = false;
        const int requestedDelay = delayIndex >= 0 && delayIndex + 1 < arguments.size()
            ? arguments.at(delayIndex + 1).toInt(&delayIsValid) : 230;
        controller.renderPreview(arguments.at(previewIndex + 1), section,
                                 delayIsValid ? qBound(50, requestedDelay, 10000) : 230);
    }
    return app.exec();
}
