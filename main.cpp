#include "app_controller.h"
#include "emoji_repository.h"
#include "single_instance.h"
#include "usage_store.h"

#include <QApplication>
#include <QFont>
#include <QMessageBox>
#include <QPixmapCache>
#include <QStringList>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QPixmapCache::setCacheLimit(4096);
    app.setApplicationName(QStringLiteral("PickMoji"));
    app.setApplicationDisplayName(QStringLiteral("PickMoji"));
    app.setOrganizationName(QStringLiteral("PickMoji"));
    app.setOrganizationDomain(QStringLiteral("pickmoji.app"));
    app.setFont(QFont(QStringLiteral("Segoe UI"), 10));

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
    const QStringList arguments = app.arguments();
    // TEMPORARY: anchor logging is on by default while we diagnose placement.
    // Revert to opt-in (--debug-anchor) once positioning is settled.
    controller.setDebugAnchor(!arguments.contains(QStringLiteral("--no-anchor-log")));
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
