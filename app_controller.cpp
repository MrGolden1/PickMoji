#include "app_controller.h"

#include "emoji_repository.h"
#include "single_instance.h"
#include "usage_store.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QDialog>
#include <QFile>
#include <QTextStream>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmapCache>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr auto ORGANIZATION = "PickMoji";
constexpr auto APPLICATION = "PickMoji";
constexpr auto RUN_KEY = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// The picker takes no focus, so outside clicks are detected by sampling the
// pointer. Poll briskly while it is on screen, lazily while it is hidden.
constexpr int POLL_VISIBLE_MS = 40;
constexpr int POLL_HIDDEN_MS = 300;
constexpr int ANCHOR_GAP = 14;
// Hard ceiling on how long we will wait for a UI Automation answer. A busy or
// hung target app must delay the picker, never freeze it. This is generous
// because the *first* query against a Chromium/Electron app also pays for that
// app building its accessibility tree; a warm query answers in ~10ms, and an app
// with no caret returns NotFound promptly rather than timing out.
constexpr int CARET_QUERY_TIMEOUT_MS = 300;
}

AppController::AppController(const EmojiRepository *repository, UsageStore *usage,
                             SingleInstance *singleInstance, QObject *parent)
    : QObject(parent), m_picker(repository, usage), m_usage(usage),
      m_singleInstance(singleInstance), m_tray(this) {
    m_picker.setWindowIcon(createAppIcon());
    qApp->setWindowIcon(m_picker.windowIcon());

    connect(&m_picker, &PickerWindow::emojiChosen, this, &AppController::chooseEmoji);
    connect(&m_picker, &PickerWindow::hiddenByUser, this, [this]() {
        if (m_showAction)
            m_showAction->setText("Open PickMoji");
        m_hotkey.unregisterDismissKey();
        m_targetMonitor.setInterval(POLL_HIDDEN_MS);
        scheduleMemoryTrim();
    });
    connect(&m_picker, &PickerWindow::searchFocusRequested, this, &AppController::enterPickerTyping);
    connect(&m_picker, &PickerWindow::panelSizeChanged, this, [this](int index) {
        if (index >= 0 && index < m_sizeActions.size())
            m_sizeActions.at(index)->setChecked(true);
        if (m_picker.isVisible())
            m_picker.move(clampToScreen(m_picker.pos()));
    });
    connect(&m_hotkey, &GlobalHotkey::activated, this, &AppController::togglePicker);
    connect(&m_hotkey, &GlobalHotkey::dismissPressed, this, [this]() {
        if (m_picker.isVisible())
            m_picker.dismiss();
    });
    connect(m_singleInstance, &SingleInstance::showRequested, this, &AppController::showPicker);
    connect(qApp, &QCoreApplication::aboutToQuit, m_usage, &UsageStore::flush);

    m_targetMonitor.setInterval(300);
    connect(&m_targetMonitor, &QTimer::timeout, this, &AppController::onMonitorTick);
}

void AppController::start(bool backgroundOnly) {
    QApplication::setQuitOnLastWindowClosed(false);
    setupTray();
    if (followTextCursorEnabled())
        m_windows.warmUpCaretQuery();

    QSettings settings(ORGANIZATION, APPLICATION);
    const QString savedText = settings.value(
        "globalShortcut", GlobalHotkey::defaultShortcut().toString(QKeySequence::PortableText)).toString();
    QKeySequence requested = QKeySequence::fromString(savedText, QKeySequence::PortableText);
    if (requested.isEmpty())
        requested = GlobalHotkey::defaultShortcut();
    if (!m_hotkey.registerShortcut(requested)) {
        m_hotkeyWarning = m_hotkey.errorString();
        if (requested != GlobalHotkey::defaultShortcut()
            && m_hotkey.registerShortcut(GlobalHotkey::defaultShortcut())) {
            m_hotkeyWarning += QStringLiteral(" Using Alt+. for this session.");
        }
    }
    updateShortcutUi();
    m_targetMonitor.start();
    updateLastTarget();

    if (!backgroundOnly)
        showPicker();
    else
        scheduleMemoryTrim();

    const QString warning = m_hotkeyWarning.isEmpty() ? m_hotkey.errorString() : m_hotkeyWarning;
    if (!warning.isEmpty() && m_tray.isVisible()) {
        m_tray.showMessage("PickMoji", warning,
                           QSystemTrayIcon::Warning, 4500);
    }
}

void AppController::renderPreview(const QString &path, const QString &sectionId, int captureDelayMs) {
    showPicker();
    m_picker.suspendAutoHideForInsertion();
    QTimer::singleShot(120, this, [this, path, sectionId, captureDelayMs]() {
        if (!sectionId.isEmpty())
            m_picker.scrollToSection(sectionId);
        QTimer::singleShot(captureDelayMs, this, [this, path]() {
            const qreal scale = m_picker.devicePixelRatioF();
            const QSize pixelSize = (QSizeF(m_picker.size()) * scale).toSize();
            QPixmap capture(pixelSize);
            capture.setDevicePixelRatio(scale);
            capture.fill(Qt::transparent);
            m_picker.render(&capture);
            capture.save(path, "PNG");
            qApp->quit();
        });
    });
}

void AppController::setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_trayMenu = new QMenu;
    m_showAction = m_trayMenu->addAction("Open PickMoji");
    connect(m_showAction, &QAction::triggered, this, &AppController::togglePicker);

    m_shortcutAction = m_trayMenu->addAction("Keyboard shortcut…");
    connect(m_shortcutAction, &QAction::triggered, this, &AppController::showShortcutDialog);

    QMenu *sizeMenu = m_trayMenu->addMenu("Panel size");
    m_sizeGroup = new QActionGroup(this);
    m_sizeGroup->setExclusive(true);
    const QStringList sizeLabels = PickerWindow::panelSizeLabels();
    for (int i = 0; i < sizeLabels.size(); ++i) {
        QAction *sizeAction = sizeMenu->addAction(sizeLabels.at(i));
        sizeAction->setCheckable(true);
        sizeAction->setChecked(i == m_picker.panelSizeIndex());
        m_sizeGroup->addAction(sizeAction);
        m_sizeActions.append(sizeAction);
        connect(sizeAction, &QAction::triggered, this, [this, i]() { m_picker.setPanelSizeIndex(i); });
    }

    m_trayMenu->addSeparator();
    m_compatibilityAction = m_trayMenu->addAction("Compatibility paste mode");
    m_compatibilityAction->setCheckable(true);
    m_compatibilityAction->setToolTip("Use clipboard paste for apps that reject direct Unicode input");
    m_compatibilityAction->setChecked(compatibilityPasteEnabled());
    connect(m_compatibilityAction, &QAction::toggled,
            this, &AppController::setCompatibilityPasteEnabled);

    m_followCursorAction = m_trayMenu->addAction("Follow text cursor");
    m_followCursorAction->setCheckable(true);
    m_followCursorAction->setToolTip(
        "Open the picker at the text cursor. Uses accessibility APIs, which can make "
        "Chromium/Electron apps build an accessibility tree. Turn off to always open "
        "at the mouse pointer.");
    m_followCursorAction->setChecked(followTextCursorEnabled());
    connect(m_followCursorAction, &QAction::toggled,
            this, &AppController::setFollowTextCursorEnabled);

    m_startupAction = m_trayMenu->addAction("Start with Windows");
    m_startupAction->setCheckable(true);
    m_startupAction->setChecked(startsWithWindows());
    connect(m_startupAction, &QAction::toggled, this, &AppController::setStartWithWindows);

    m_trayMenu->addSeparator();
    QAction *exitAction = m_trayMenu->addAction("Exit");
    connect(exitAction, &QAction::triggered, qApp, &QCoreApplication::quit);

    m_tray.setIcon(createAppIcon());
    m_tray.setToolTip("PickMoji");
    m_tray.setContextMenu(m_trayMenu);
    connect(&m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            togglePicker();
    });
    m_tray.show();
}

void AppController::showShortcutDialog() {
    QDialog dialog;
    dialog.setWindowTitle("Keyboard Shortcut");
    dialog.setWindowIcon(m_picker.windowIcon());
    dialog.setMinimumWidth(370);

    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(
        "Press one global shortcut. A modifier plus one key works best.", &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *editor = new QKeySequenceEdit(m_hotkey.shortcut(), &dialog);
    editor->setMaximumSequenceLength(1);
    editor->setClearButtonEnabled(true);
    layout->addWidget(editor);

    auto *hint = new QLabel(
        "Default: Alt + .   Windows-reserved or already-used shortcuts cannot be saved.", &dialog);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #687987;");
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QPushButton *defaultButton = buttons->addButton("Use Alt + .", QDialogButtonBox::ResetRole);
    layout->addWidget(buttons);
    connect(defaultButton, &QPushButton::clicked, editor, [editor]() {
        editor->setKeySequence(GlobalHotkey::defaultShortcut());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [this, &dialog, editor]() {
        const QKeySequence shortcut = editor->keySequence();
        if (!m_hotkey.registerShortcut(shortcut)) {
            QMessageBox::warning(&dialog, "Shortcut unavailable", m_hotkey.errorString());
            return;
        }
        QSettings settings(ORGANIZATION, APPLICATION);
        settings.setValue("globalShortcut", shortcut.toString(QKeySequence::PortableText));
        m_hotkeyWarning.clear();
        updateShortcutUi();
        dialog.accept();
    });
    dialog.exec();
}

void AppController::updateShortcutUi() {
    const QString label = m_hotkey.shortcut().isEmpty()
        ? QStringLiteral("Not set")
        : m_hotkey.shortcut().toString(QKeySequence::NativeText);
    m_picker.setShortcutHint(label);
    if (m_shortcutAction)
        m_shortcutAction->setText(QStringLiteral("Keyboard shortcut…    %1").arg(label));
    if (m_tray.isVisible())
        m_tray.setToolTip(QStringLiteral("PickMoji · %1").arg(label));
}

void AppController::updateLastTarget() {
    if (m_picker.isActiveWindow())
        return;
    const quintptr foreground = m_windows.foregroundWindow();
    const quintptr pickerHandle = static_cast<quintptr>(m_picker.winId());
    if (m_windows.isUsableTarget(foreground, pickerHandle))
        m_lastTarget = foreground;
}

void AppController::showPicker() {
    const quintptr pickerHandle = static_cast<quintptr>(m_picker.winId());
    const quintptr foreground = m_windows.foregroundWindow();
    if (m_windows.isUsableTarget(foreground, pickerHandle))
        m_lastTarget = foreground;
    m_activeTarget = m_lastTarget;
    // Remember the foreground at open time so the click-away watchdog can tell
    // "still using the target app" from "switched to another window".
    m_openForeground = foreground;

    m_windows.setWindowNoActivate(pickerHandle, true);
    m_picker.prepareForShow();
    m_picker.move(boundedPickerPosition(anchorForPicker()));
    m_picker.show();
    m_picker.raise();
    m_targetMonitor.setInterval(POLL_VISIBLE_MS);
    // Escape belongs to us only while the panel is up; released again on hide so
    // it immediately behaves normally in every other app.
    m_hotkey.registerDismissKey();
    // Intentionally no activateWindow(): the target app keeps keyboard focus so
    // clicking an emoji injects straight into it, with no foreground bounce.
    if (m_showAction)
        m_showAction->setText("Hide PickMoji");
}

void AppController::togglePicker() {
    if (m_picker.isVisible())
        m_picker.dismiss(); // routes through hiddenByUser: menu text, poll rate, trim
    else
        showPicker();
}

QPoint AppController::boundedPickerPosition(const QPoint &anchor) const {
    // Place the panel next to the anchor (caret or pointer) without ever
    // covering it: below if it fits, else above, else beside it. Everything is
    // resolved on the anchor's own screen, so the picker follows the monitor the
    // user is actually working on.
    QScreen *screen = QGuiApplication::screenAt(anchor);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const QSize pickerSize = m_picker.size();

    const int x = std::clamp(anchor.x() - pickerSize.width() / 2,
                             available.left(), available.right() - pickerSize.width() + 1);

    const int below = anchor.y() + ANCHOR_GAP;
    if (below + pickerSize.height() - 1 <= available.bottom())
        return QPoint(x, below);

    const int above = anchor.y() - ANCHOR_GAP - pickerSize.height();
    if (above >= available.top())
        return QPoint(x, above);

    // Too tall to fit above or below. Sitting *adjacent* to the anchor is wrong:
    // the panel still lands on the line being typed, just to one side of the
    // caret. Retreat to whichever screen edge has more room instead, so the text
    // column the user is working in stays fully visible.
    const int y = std::clamp(anchor.y() - pickerSize.height() / 2,
                             available.top(), available.bottom() - pickerSize.height() + 1);
    const int roomLeft = anchor.x() - available.left();
    const int roomRight = available.right() - anchor.x();
    const int sideX = (roomRight >= roomLeft) ? available.right() - pickerSize.width() + 1
                                              : available.left();
    return QPoint(sideX, y);
}

bool AppController::isPlausibleAnchor(const QRect &rect) const {
    if (rect.height() <= 0)
        return false;
    QScreen *screen = QGuiApplication::screenAt(rect.topLeft());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return false;
    const QRect available = screen->availableGeometry();
    if (!available.intersects(rect))
        return false; // stale or off-screen
    // Focus can land on a whole document/canvas rather than a text line. Such a
    // rect is useless as an anchor, so reject it and let the next source win.
    return rect.height() <= available.height() / 2;
}

QPoint AppController::anchorForPicker() const {
    QStringList trace;
    QPoint chosen;
    bool haveChosen = false;

    if (followTextCursorEnabled()) {
        // 1) Classic Win32 caret: free and side-effect-free, so try it first.
        QPoint caret;
        // Some apps leave a stale/hidden caret behind, so require it to actually
        // land on a screen before trusting it.
        if (m_windows.caretPosition(m_activeTarget, caret)
            && QGuiApplication::screenAt(caret) != nullptr) {
            trace << QStringLiteral("win32Caret=HIT(%1,%2)").arg(caret.x()).arg(caret.y());
            chosen = caret;
            haveChosen = true;
        } else {
            trace << QStringLiteral("win32Caret=MISS");
        }

        // 2) UI Automation: the only way to find the caret in Chromium/Electron/
        //    UWP apps. Deliberately second, because querying it makes those apps
        //    build an accessibility tree (a real cost in *their* process).
        if (!haveChosen) {
            QRect textRect;
            int elapsed = 0;
            const WindowsIntegration::CaretQuery status =
                m_windows.focusedTextRect(textRect, CARET_QUERY_TIMEOUT_MS, &elapsed);
            const char *statusText = "?";
            switch (status) {
            case WindowsIntegration::CaretQuery::Found: statusText = "FOUND"; break;
            case WindowsIntegration::CaretQuery::NotFound: statusText = "NOTFOUND"; break;
            case WindowsIntegration::CaretQuery::TimedOut: statusText = "TIMEOUT"; break;
            case WindowsIntegration::CaretQuery::Unsupported: statusText = "UNSUPPORTED"; break;
            }
            trace << QStringLiteral("uia=%1 in %2ms rect=(%3,%4 %5x%6) plausible=%7")
                         .arg(QLatin1String(statusText)).arg(elapsed)
                         .arg(textRect.x()).arg(textRect.y())
                         .arg(textRect.width()).arg(textRect.height())
                         .arg(isPlausibleAnchor(textRect) ? "yes" : "no");
            if (status == WindowsIntegration::CaretQuery::Found && isPlausibleAnchor(textRect)) {
                chosen = QPoint(textRect.left(), textRect.bottom());
                haveChosen = true;
            }
        }
    } else {
        trace << QStringLiteral("followTextCursor=off");
    }

    // 3) The pointer: always available, and on the monitor the user is using.
    if (!haveChosen)
        chosen = QCursor::pos();

    if (m_debugAnchor) {
        trace << QStringLiteral("source=%1 anchor=(%2,%3) panelSize=%4x%5")
                     .arg(haveChosen ? "caret" : "mouse")
                     .arg(chosen.x()).arg(chosen.y())
                     .arg(m_picker.width()).arg(m_picker.height());
        QFile log(QDir::tempPath() + QStringLiteral("/pickmoji-anchor.log"));
        if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream(&log) << trace.join(QStringLiteral(" | ")) << '\n';
        }
    }
    return chosen;
}

QPoint AppController::clampToScreen(const QPoint &topLeft) const {
    QScreen *screen = QGuiApplication::screenAt(topLeft);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const QSize pickerSize = m_picker.size();
    const int x = std::clamp(topLeft.x(), available.left(), available.right() - pickerSize.width() + 1);
    const int y = std::clamp(topLeft.y(), available.top(), available.bottom() - pickerSize.height() + 1);
    return QPoint(x, y);
}

void AppController::enterPickerTyping() {
    // Focus-on-demand: temporarily allow activation, pull the picker forward and
    // hand keyboard focus to the search field. Insertion later restores the
    // target and returns the picker to passive mode.
    const quintptr pickerHandle = static_cast<quintptr>(m_picker.winId());
    m_windows.setWindowNoActivate(pickerHandle, false);
    m_windows.activateTarget(pickerHandle);
    m_picker.beginTypingMode();
}

void AppController::onMonitorTick() {
    if (!m_picker.isVisible()) {
        updateLastTarget();
        return;
    }
    // The tone palette lives outside the picker's own rect; typing mode is a real
    // focused window and dismisses itself on deactivation.
    if (m_picker.isTypingMode() || m_picker.isVariantMenuOpen())
        return;

    // Click-outside dismissal. The picker never takes focus, so clicking back
    // into the target app raises no deactivation event and does not even change
    // the foreground window — the only reliable signal is the pointer itself.
    if (m_windows.isPointerButtonDown()
        && !m_picker.frameGeometry().contains(QCursor::pos())) {
        m_picker.dismiss();
        return;
    }

    // Belt and braces: Alt+Tab or any other switch away from the app we opened
    // over also dismisses.
    const quintptr pickerHandle = static_cast<quintptr>(m_picker.winId());
    const quintptr foreground = m_windows.foregroundWindow();
    if (foreground && foreground != pickerHandle && foreground != m_openForeground)
        m_picker.dismiss();
}


void AppController::chooseEmoji(const QString &emoji, bool copyOnly) {
    if (copyOnly) {
        QGuiApplication::clipboard()->setText(emoji);
        return;
    }

    const quintptr target = m_activeTarget;
    m_picker.suspendAutoHideForInsertion();

    QTimer::singleShot(25, this, [this, target, emoji]() {
        const quintptr pickerHandle = static_cast<quintptr>(m_picker.winId());
        const bool hadTarget = m_windows.isUsableTarget(target, pickerHandle);
        const bool inserted = hadTarget
            && m_windows.insertText(target, emoji, compatibilityPasteEnabled());
        if (!inserted) {
            QGuiApplication::clipboard()->setText(emoji);
            // Only notify when a real target actively rejected the input. When
            // nothing usable was focused, silently copy instead of nagging.
            if (hadTarget && m_tray.isVisible()) {
                m_tray.showMessage("Emoji copied",
                                   "The target app rejected input, so the emoji was copied instead.",
                                   QSystemTrayIcon::Information, 2600);
            }
        }
        // Return the picker to its passive, non-activating state so the next
        // click inserts without a focus round-trip. The target is foreground now
        // (insertText restored it if search had taken focus), so update the
        // watchdog baseline to match.
        m_windows.setWindowNoActivate(pickerHandle, true);
        m_openForeground = m_windows.foregroundWindow();
        QTimer::singleShot(60, this, [this]() { m_picker.resumeAfterInsertion(); });
    });
}

void AppController::scheduleMemoryTrim() {
    QTimer::singleShot(300, this, [this]() {
        if (!m_picker.isVisible()) {
            QPixmapCache::clear();
            m_windows.trimWorkingSet();
        }
    });
}

QIcon AppController::createAppIcon() const {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#3390ec"));
    p.drawEllipse(QRectF(3, 3, 58, 58));
    p.setPen(QPen(Qt::white, 4.2, Qt::SolidLine, Qt::RoundCap));
    p.drawPoint(QPointF(23, 26));
    p.drawPoint(QPointF(41, 26));
    p.drawArc(QRectF(17, 22, 30, 25), 205 * 16, 130 * 16);
    return QIcon(pixmap);
}

bool AppController::compatibilityPasteEnabled() const {
    QSettings settings(ORGANIZATION, APPLICATION);
    return settings.value("compatibilityPaste", false).toBool();
}

void AppController::setCompatibilityPasteEnabled(bool enabled) {
    QSettings settings(ORGANIZATION, APPLICATION);
    settings.setValue("compatibilityPaste", enabled);
}

bool AppController::followTextCursorEnabled() const {
    QSettings settings(ORGANIZATION, APPLICATION);
    return settings.value("followTextCursor", true).toBool();
}

void AppController::setFollowTextCursorEnabled(bool enabled) {
    QSettings settings(ORGANIZATION, APPLICATION);
    settings.setValue("followTextCursor", enabled);
}

bool AppController::startsWithWindows() const {
#ifdef Q_OS_WIN
    QSettings runSettings(RUN_KEY, QSettings::NativeFormat);
    return runSettings.contains(QStringLiteral("PickMoji"));
#else
    return false;
#endif
}

void AppController::setStartWithWindows(bool enabled) {
#ifdef Q_OS_WIN
    QSettings runSettings(RUN_KEY, QSettings::NativeFormat);
    const QString name = QStringLiteral("PickMoji");
    if (enabled) {
        const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        runSettings.setValue(name, QStringLiteral("\"") + executable + QStringLiteral("\" --background"));
    } else {
        runSettings.remove(name);
    }
#else
    Q_UNUSED(enabled);
#endif
}
