#include "emoji_repository.h"
#include "windows_integration.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QLineEdit>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QFile::remove(QCoreApplication::applicationDirPath() + "/smoke-test-result.txt");

    EmojiRepository repository;
    if (!repository.load() || repository.entries().size() != 3944
        || repository.groups().size() != 9
        || repository.indexOf(QStringLiteral("🧑‍🚀")) < 0
        || repository.indicesForGroup(QStringLiteral("Flags")).size() < 250) {
        qCritical() << "Repository validation failed" << repository.errorString()
                    << repository.entries().size() << repository.groups().size();
        return 2;
    }

    const QPixmap iranFlag(QStringLiteral(":/flags/1f1ee-1f1f7.png"));
    if (iranFlag.isNull() || iranFlag.width() < 32)
        return 4;

    const int wavingHand = repository.indexOf(QStringLiteral("👋"));
    const int handshake = repository.indexOf(QStringLiteral("🤝"));
    const int bunnyEars = repository.indexOf(QStringLiteral("👯"));
    const QVector<int> &visiblePeople = repository.indicesForGroup(QStringLiteral("People & Body"));
    if (wavingHand < 0 || repository.skinToneVariantsFor(wavingHand).size() != 6
        || handshake < 0 || repository.skinToneVariantsFor(handshake).size() != 26
        || bunnyEars < 0 || repository.skinToneVariantsFor(bunnyEars).size() != 26
        || visiblePeople.size() >= 450)
        return 6;
    for (int index : visiblePeople) {
        if (repository.entries().at(index).isSkinToneVariant)
            return 7;
    }

    GlobalHotkey hotkey;
    if (!hotkey.registerShortcut(GlobalHotkey::defaultShortcut()))
        return 5;
    const QKeySequence customShortcut(
        QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_F12));
    if (!hotkey.registerShortcut(customShortcut)
        || !hotkey.registerShortcut(GlobalHotkey::defaultShortcut()))
        return 8;

    QWidget window;
    window.setWindowTitle(QStringLiteral("Emoji Picker Input Smoke Test"));
    window.resize(360, 90);
    auto *layout = new QVBoxLayout(&window);
    auto *edit = new QLineEdit(&window);
    layout->addWidget(edit);
    window.show();
    window.activateWindow();
    edit->setFocus(Qt::OtherFocusReason);

    auto *integration = new WindowsIntegration(&app);
    const QString expected = QStringLiteral("🧑‍🚀");
    QTimer::singleShot(180, &app, [&app, &window, edit, integration, expected]() {
        const bool sent = integration->insertText(static_cast<quintptr>(window.winId()), expected, false);
        QTimer::singleShot(250, &app, [&app, edit, expected, sent]() {
            const bool valid = sent && edit->text() == expected;
            if (!valid) {
                qCritical() << "Input validation failed" << "sent=" << sent
                            << "actual=" << edit->text().toUtf8().toHex()
                            << "expected=" << expected.toUtf8().toHex();
                QFile details(QCoreApplication::applicationDirPath() + "/smoke-test-result.txt");
                if (details.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    details.write("sent=");
                    details.write(sent ? "true\n" : "false\n");
                    details.write("actual=");
                    details.write(edit->text().toUtf8().toHex());
                    details.write("\nexpected=");
                    details.write(expected.toUtf8().toHex());
                    details.write("\n");
                }
            }
            // Allow the production clipboard-preservation timer to restore all
            // previous MIME formats before the test process exits.
            QTimer::singleShot(850, &app, [&app, valid]() { app.exit(valid ? 0 : 3); });
        });
    });
    return app.exec();
}
