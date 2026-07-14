#pragma once

#include "picker_window.h"
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

public slots:
    void showPicker();
    void togglePicker();

private:
    void setupTray();
    void showShortcutDialog();
    void updateShortcutUi();
    void chooseEmoji(const QString &emoji, bool copyOnly);
    void updateLastTarget();
    void onMonitorTick();
    void enterPickerTyping();
    void scheduleMemoryTrim();
    QPoint boundedPickerPosition(const QPoint &anchor) const;
    QPoint clampToScreen(const QPoint &topLeft) const;
    QPoint anchorForPicker() const;
    bool isPlausibleAnchor(const QRect &rect) const;
    QIcon createAppIcon() const;
    bool compatibilityPasteEnabled() const;
    void setCompatibilityPasteEnabled(bool enabled);
    bool followTextCursorEnabled() const;
    void setFollowTextCursorEnabled(bool enabled);
    bool startsWithWindows() const;
    void setStartWithWindows(bool enabled);

    PickerWindow m_picker;
    WindowsIntegration m_windows;
    GlobalHotkey m_hotkey;
    UsageStore *m_usage = nullptr;
    SingleInstance *m_singleInstance = nullptr;
    QSystemTrayIcon m_tray;
    QMenu *m_trayMenu = nullptr;
    QAction *m_showAction = nullptr;
    QAction *m_shortcutAction = nullptr;
    QAction *m_compatibilityAction = nullptr;
    QAction *m_followCursorAction = nullptr;
    QAction *m_startupAction = nullptr;
    QActionGroup *m_sizeGroup = nullptr;
    QList<QAction *> m_sizeActions;
    QTimer m_targetMonitor;
    quintptr m_lastTarget = 0;
    quintptr m_activeTarget = 0;
    quintptr m_openForeground = 0;
    QString m_hotkeyWarning;
};
