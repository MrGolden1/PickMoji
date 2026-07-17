#pragma once

#include "picker_window.h"
#include "update_checker.h"
#include "windows_integration.h"

#include <QAction>
#include <QList>
#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>

class EmojiRepository;
class QActionGroup;
class QMenu;
class SingleInstance;
class UsageStore;

class AppController final : public QObject {
    Q_OBJECT

public:
    AppController(const EmojiRepository *repository, UsageStore *usage,
                  SingleInstance *singleInstance, QObject *parent = nullptr);
    void start(bool backgroundOnly);
    void renderPreview(const QString &path, const QString &sectionId = {}, int captureDelayMs = 230);
    // Opt-in (--debug-anchor): append the anchor decision to %TEMP%/pickmoji-anchor.log
    void setDebugAnchor(bool enabled) { m_debugAnchor = enabled; }

public slots:
    void showPicker();
    void togglePicker();

private:
    void setupTray();
    void showShortcutDialog();
    void showAboutDialog();
    void updateShortcutUi();
    void chooseEmoji(const QString &emoji, bool copyOnly);
    void updateLastTarget();
    void onMonitorTick();
    void enterPickerTyping();
    void scheduleMemoryTrim();
    QPoint clampToScreen(const QPoint &topLeft) const;
    // The text caret's region: the panel is placed around the mouse, but must
    // not land on top of this.
    QRect keepClearRect(QStringList *trace) const;
    QPoint pickerPosition(const QPoint &pointer, const QRect &keepClear,
                          QStringList *trace) const;
    bool isPlausibleKeepClear(const QRect &rect) const;
    QIcon createAppIcon() const;
    bool compatibilityPasteEnabled() const;
    void setCompatibilityPasteEnabled(bool enabled);
    bool followTextCursorEnabled() const;
    void setFollowTextCursorEnabled(bool enabled);
    bool startsWithWindows() const;
    void setStartWithWindows(bool enabled);
    bool autoUpdateEnabled() const;
    void setAutoUpdateEnabled(bool enabled);
    void maybeAutoCheckForUpdates();
    void onUpdateAvailable(const QString &version);

    PickerWindow m_picker;
    WindowsIntegration m_windows;
    GlobalHotkey m_hotkey;
    UpdateChecker m_updater;
    UsageStore *m_usage = nullptr;
    SingleInstance *m_singleInstance = nullptr;
    QSystemTrayIcon m_tray;
    QMenu *m_trayMenu = nullptr;
    QAction *m_showAction = nullptr;
    QAction *m_shortcutAction = nullptr;
    QAction *m_compatibilityAction = nullptr;
    QAction *m_followCursorAction = nullptr;
    QAction *m_startupAction = nullptr;
    QAction *m_checkUpdateAction = nullptr;
    QAction *m_autoUpdateAction = nullptr;
    bool m_updatePromptPending = false;
    QActionGroup *m_sizeGroup = nullptr;
    QList<QAction *> m_sizeActions;
    QTimer m_targetMonitor;
    quintptr m_lastTarget = 0;
    quintptr m_activeTarget = 0;
    quintptr m_openForeground = 0;
    // Last sampled pointer-button state: dismissal must trigger only on a press
    // that *begins* outside the panel, or dragging the scrollbar (or the panel
    // itself) past the frame's edge would count as a click-away.
    bool m_pointerWasDown = false;
    bool m_debugAnchor = false;
    QString m_hotkeyWarning;
};
