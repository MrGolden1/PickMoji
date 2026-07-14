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
#include <limits>
#include <utility>

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

int overlapArea(const QRect &a, const QRect &b) {
    const QRect intersection = a.intersected(b);
    return intersection.isEmpty() ? 0 : intersection.width() * intersection.height();
}
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

    if (m_debugAnchor) {
        QFile log(QDir::tempPath() + QStringLiteral("/pickmoji-anchor.log"));
        if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream stream(&log);
            const QStringList lines = m_windows.describeScreens();
            for (const QString &line : lines)
                stream << line << '\n';
        }
    }

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

    // The pointer decides where the panel goes; the caret is a region it must
    // not cover.
    QStringList trace;
    const QRect keepClear = keepClearRect(&trace);
    const QPoint pointer = QCursor::pos();
    const QPoint topLeft = pickerPosition(pointer, keepClear, &trace);
    if (m_debugAnchor) {
        trace << QStringLiteral("pointer=(%1,%2) panel=%3x%4")
                      .arg(pointer.x()).arg(pointer.y())
                      .arg(m_picker.width()).arg(m_picker.height());
        QFile log(QDir::tempPath() + QStringLiteral("/pickmoji-anchor.log"));
        if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            QTextStream(&log) << trace.join(QStringLiteral(" | ")) << '\n';
    }

    m_windows.setWindowNoActivate(pickerHandle, true);
    m_picker.prepareForShow();
    m_picker.move(topLeft);
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

QPoint AppController::pickerPosition(const QPoint &pointer, const QRect &keepClear,
                                     QStringList *trace) const {
    // The pointer decides *where* the panel wants to be; the caret decides where
    // it may not go. We walk candidate placements around the pointer and take the
    // first that both fits on screen and leaves the caret visible.
    QScreen *screen = QGuiApplication::screenAt(pointer);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const QSize size = m_picker.size();
    const int w = size.width();
    const int h = size.height();
    const int gap = ANCHOR_GAP;

    QRect forbidden;
    if (keepClear.isValid() && keepClear.height() > 0)
        forbidden = keepClear.adjusted(-gap, -gap, gap, gap); // a little breathing room

    if (trace) {
        *trace << QStringLiteral("screen=(%1,%2 %3x%4) keepClear=%5")
                      .arg(available.x()).arg(available.y())
                      .arg(available.width()).arg(available.height())
                      .arg(forbidden.isNull() ? QStringLiteral("NONE")
                                              : QStringLiteral("(%1,%2 %3x%4)")
                                                    .arg(forbidden.x()).arg(forbidden.y())
                                                    .arg(forbidden.width())
                                                    .arg(forbidden.height()));
    }

    // Keep every candidate on screen, then re-test it. Sliding a candidate back
    // onto the screen is fine and looks natural; what matters is that the overlap
    // check happens *after* the slide, so clamping can never silently re-cover
    // the caret. (Requiring a perfect fit instead is what used to force the panel
    // out to a screen corner.)
    const int maxX = std::max(available.left(), available.right() - w + 1);
    const int maxY = std::max(available.top(), available.bottom() - h + 1);
    const auto onScreen = [&](const QPoint &p) {
        return QPoint(std::clamp(p.x(), available.left(), maxX),
                      std::clamp(p.y(), available.top(), maxY));
    };

    struct Candidate {
        const char *label;
        QPoint topLeft;
    };
    QList<Candidate> candidates;

    // Preferred: hug the pointer, since that is where the user is looking.
    const int px = pointer.x() - w / 2;
    const int py = pointer.y() - h / 2;
    candidates << Candidate{"belowPointer", {px, pointer.y() + gap}}
               << Candidate{"abovePointer", {px, pointer.y() - gap - h}}
               << Candidate{"rightOfPointer", {pointer.x() + gap, py}}
               << Candidate{"leftOfPointer", {pointer.x() - gap - w, py}};

    // Then hug the caret. If the pointer leaves no room, sitting directly under
    // the text cursor is a natural home — far better than fleeing to a corner.
    if (!forbidden.isNull()) {
        const int kx = keepClear.center().x() - w / 2;
        const int ky = keepClear.center().y() - h / 2;
        candidates << Candidate{"belowCaret", {kx, keepClear.bottom() + gap}}
                   << Candidate{"aboveCaret", {kx, keepClear.top() - gap - h}}
                   << Candidate{"rightOfCaret", {keepClear.right() + gap, ky}}
                   << Candidate{"leftOfCaret", {keepClear.left() - gap - w, ky}};
    }

    QPoint best;
    QString bestLabel;
    int bestOverlap = std::numeric_limits<int>::max();

    for (const Candidate &candidate : std::as_const(candidates)) {
        const QPoint topLeft = onScreen(candidate.topLeft);
        const int overlap = overlapArea(QRect(topLeft, size), forbidden);
        if (overlap == 0) {
            if (trace) {
                *trace << QStringLiteral("place=%1(%2,%3)")
                              .arg(QLatin1String(candidate.label))
                              .arg(topLeft.x()).arg(topLeft.y());
            }
            return topLeft;
        }
        if (overlap < bestOverlap) {
            bestOverlap = overlap;
            best = topLeft;
            bestLabel = QLatin1String(candidate.label);
        }
    }

    // Nothing can clear the caret. Take the least-bad natural spot and accept
    // covering it — that beats a position the user would find bizarre.
    if (trace) {
        *trace << QStringLiteral("place=%1(%2,%3) covered=%4px2")
                      .arg(bestLabel).arg(best.x()).arg(best.y()).arg(bestOverlap);
    }
    return best;
}

bool AppController::isPlausibleKeepClear(const QRect &rect) const {
    if (rect.height() <= 0)
        return false;
    QScreen *screen = QGuiApplication::screenAt(rect.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return false;
    const QRect available = screen->availableGeometry();
    if (!available.intersects(rect))
        return false; // stale or off-screen
    // Focus can land on a whole document/canvas rather than a text line. Treating
    // that as keep-clear would banish the panel for no reason, so ignore it.
    return rect.height() <= available.height() / 2;
}

QRect AppController::keepClearRect(QStringList *trace) const {
    if (!followTextCursorEnabled()) {
        if (trace)
            *trace << QStringLiteral("followTextCursor=off");
        return {};
    }

    // 1) Classic Win32 caret: free and side-effect-free, so try it first. Some
    //    apps leave a stale caret behind, so require it to land on a screen.
    QRect caret;
    if (m_windows.caretRect(m_activeTarget, caret)
        && QGuiApplication::screenAt(caret.center()) != nullptr
        && isPlausibleKeepClear(caret)) {
        if (trace) {
            *trace << QStringLiteral("win32Caret=HIT(%1,%2 %3x%4)")
                          .arg(caret.x()).arg(caret.y())
                          .arg(caret.width()).arg(caret.height());
        }
        return caret;
    }
    if (trace)
        *trace << QStringLiteral("win32Caret=MISS");

    // 2) UI Automation: the only way to find the caret in Chromium/Electron/UWP
    //    apps. Second, because querying it makes those apps build an
    //    accessibility tree (a real cost in *their* process).
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
    const bool usable = status == WindowsIntegration::CaretQuery::Found
        && isPlausibleKeepClear(textRect);
    if (trace) {
        *trace << QStringLiteral("uia=%1 in %2ms rect=(%3,%4 %5x%6) usable=%7")
                      .arg(QLatin1String(statusText)).arg(elapsed)
                      .arg(textRect.x()).arg(textRect.y())
                      .arg(textRect.width()).arg(textRect.height())
                      .arg(usable ? "yes" : "no");
    }
    return usable ? textRect : QRect();
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
