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
    connect(&m_picker, &PickerWindow::altGestureUsed, this, [this]() {
        // Alt+click opens the tone palette. The picker holds no focus, so that
        // Alt is being delivered to the app underneath — mask it now, while it
        // is still down, or Word/Slack open their menu when the user lets go.
        m_windows.maskModifierMenu();
    });
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

    // A tray utility that must already be running to answer its hotkey is dead
    // weight after a reboot, so autostart defaults on. Applied exactly once:
    // a user who later unticks it stays unticked.
    {
        QSettings settings(ORGANIZATION, APPLICATION);
        if (!settings.value("startupDefaultApplied", false).toBool()) {
            settings.setValue("startupDefaultApplied", true);
            if (!startsWithWindows())
                setStartWithWindows(true);
        }
    }
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

    // The caret decides where the panel goes (below the line, else above); the
    // pointer is only the fallback when no caret is available.
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

    // Seed the pointer edge-detector: a button still held from the click that
    // opened us (tray icon, menu) must not read as a fresh outside press.
    m_pointerWasDown = m_windows.isPointerButtonDown();

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
    const QSize size = m_picker.size();
    const int w = size.width();
    const int h = size.height();
    const int gap = ANCHOR_GAP;
    const bool haveCaret = keepClear.isValid() && keepClear.height() > 0;

    // Anchor to the screen the user is actually working on: the caret's when we
    // have one (that is where they are typing), otherwise the pointer's. Picking
    // the pointer's screen while placing relative to a caret on another monitor
    // was a latent multi-monitor bug.
    QScreen *screen = QGuiApplication::screenAt(haveCaret ? keepClear.center() : pointer);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);

    // Slide any spot back onto the screen, then re-test overlap afterwards, so
    // clamping can never silently push the panel back over the caret.
    const int maxX = std::max(available.left(), available.right() - w + 1);
    const int maxY = std::max(available.top(), available.bottom() - h + 1);
    const auto onScreen = [&](const QPoint &p) {
        return QPoint(std::clamp(p.x(), available.left(), maxX),
                      std::clamp(p.y(), available.top(), maxY));
    };

    if (trace) {
        *trace << QStringLiteral("screen=(%1,%2 %3x%4) mode=%5")
                      .arg(available.x()).arg(available.y())
                      .arg(available.width()).arg(available.height())
                      .arg(haveCaret ? QStringLiteral("caret") : QStringLiteral("pointer"));
    }

    // Caret-first: open just below the line being typed, or above it when the
    // bottom of the screen leaves no room. This mirrors the native Windows panel
    // — the picker appears where you are typing every time, regardless of where
    // the mouse sits, which is what makes it predictable. Placing below/above
    // (never beside) also means text can grow along its line without ever sliding
    // behind the panel, so there is nothing extra to keep clear.
    if (haveCaret) {
        const QRect forbidden = keepClear.adjusted(-gap, -gap, gap, gap);
        // Left edge aligned to the caret, so the panel drops down-right (below)
        // or up-right (above) from where you are typing — its near corner sits by
        // the cursor instead of the whole panel straddling it. Clamped right when
        // the caret is close to the screen's right edge.
        const int cx = std::clamp(keepClear.left(), available.left(), maxX);

        struct Spot { const char *label; QPoint topLeft; };
        const Spot spots[] = {
            {"belowCaret", {cx, forbidden.bottom() + 1}},
            {"aboveCaret", {cx, forbidden.top() - h}},
        };

        QPoint best;
        QString bestLabel;
        int bestOverlap = std::numeric_limits<int>::max();
        for (const Spot &spot : spots) {
            const QPoint topLeft = onScreen(spot.topLeft);
            const int overlap = overlapArea(QRect(topLeft, size), forbidden);
            if (overlap == 0) {
                if (trace)
                    *trace << QStringLiteral("place=%1(%2,%3)")
                                  .arg(QLatin1String(spot.label)).arg(topLeft.x()).arg(topLeft.y());
                return topLeft;
            }
            if (overlap < bestOverlap) {
                bestOverlap = overlap;
                best = topLeft;
                bestLabel = QLatin1String(spot.label);
            }
        }
        // The panel is taller than the room both above and below the caret. Cover
        // as little of it as possible rather than jump somewhere unexpected.
        if (trace)
            *trace << QStringLiteral("place=%1(%2,%3) covered=%4px2")
                          .arg(bestLabel).arg(best.x()).arg(best.y()).arg(bestOverlap);
        return best;
    }

    // No caret (following is off, or the focused app exposes none): fall back to
    // the pointer. There is nothing to keep clear, so sit below it — or above it
    // when the pointer is near the bottom edge.
    QPoint spot(pointer.x() - w / 2, pointer.y() + gap);
    if (spot.y() + h > available.bottom() + 1)
        spot.setY(pointer.y() - gap - h);
    const QPoint topLeft = onScreen(spot);
    if (trace)
        *trace << QStringLiteral("place=pointer(%1,%2)").arg(topLeft.x()).arg(topLeft.y());
    return topLeft;
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
    // Track the pointer-button edge even while a popup menu is open, so a press
    // that merely closed the menu is not mistaken for a fresh one afterwards.
    const bool pointerDown = m_windows.isPointerButtonDown();
    const bool pressStarted = pointerDown && !m_pointerWasDown;
    m_pointerWasDown = pointerDown;

    // The tone palette and the recents menu live outside the picker's own rect;
    // while one is open the pointer roams legitimately.
    if (m_picker.isVariantMenuOpen())
        return;

    // Click-outside dismissal, in *both* modes. Passive mode has no deactivation
    // event to hang it on — the pointer is the only signal. Typing mode normally
    // hides itself on WindowDeactivate, but when the activation hand-off failed
    // (SetForegroundWindow can silently refuse), that event never comes and the
    // panel would be stuck; the pointer check is the backstop that always works.
    // Edge-triggered: only a press *starting* outside dismisses — a drag that
    // began inside (scrollbar, header) may leave the frame and come back.
    if (pressStarted && !m_picker.frameGeometry().contains(QCursor::pos())) {
        m_picker.dismiss();
        return;
    }

    // Typing mode is (nominally) a real focused window; the foreground check
    // below would see the picker itself and misfire, so stop here.
    if (m_picker.isTypingMode())
        return;

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
        // No usable target, or the app swallowed the input: do nothing, like
        // the native Windows panel. Hijacking the clipboard uninvited would
        // clobber whatever the user had copied, and the toast was noise.
        // Right-click / Shift+click remain the explicit ways to copy.
        if (m_windows.isUsableTarget(target, pickerHandle))
            m_windows.insertText(target, emoji, compatibilityPasteEnabled());
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
