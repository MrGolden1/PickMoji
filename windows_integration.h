#pragma once

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QKeySequence>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

class GlobalHotkey final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit GlobalHotkey(QObject *parent = nullptr);
    ~GlobalHotkey() override;

    bool registerShortcut(const QKeySequence &shortcut);
    QKeySequence shortcut() const { return m_shortcut; }
    static QKeySequence defaultShortcut();
    QString errorString() const { return m_errorString; }

    // Claim Escape only while the picker is on screen. Windows consumes a
    // registered hotkey, so Escape dismisses the picker without also reaching
    // the app underneath — a keyboard hook would be the only other way, and we
    // deliberately avoid one. Released again as soon as the picker hides.
    bool registerDismissKey();
    void unregisterDismissKey();

signals:
    void activated();
    void dismissPressed();

protected:
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    bool m_registered = false;
    bool m_dismissRegistered = false;
    QKeySequence m_shortcut;
    QString m_errorString;
    static constexpr int HOTKEY_ID = 0x45A1;
    static constexpr int DISMISS_HOTKEY_ID = 0x45A2;
};

class WindowsIntegration final : public QObject {
    Q_OBJECT

public:
    explicit WindowsIntegration(QObject *parent = nullptr);

    quintptr foregroundWindow() const;
    bool isUsableTarget(quintptr window, quintptr pickerWindow = 0) const;
    bool isForeground(quintptr window) const;
    // The picker never takes focus, so there is no deactivation event to hang
    // click-outside dismissal on; sample the pointer buttons instead.
    bool isPointerButtonDown() const;
    QPoint caretOrCursorPosition(quintptr targetWindow) const;
    // The classic Win32 caret, as a rect so the caller can treat it as a region
    // to keep clear rather than just a point to anchor on.
    bool caretRect(quintptr targetWindow, QRect &logicalRect) const;

    // Locates the caret via UI Automation, which works in Chromium/Electron/UWP
    // apps that have no Win32 caret. Falls back to the focused control's own box
    // when the app exposes no text pattern. Runs off-thread with a deadline: a
    // busy or hung target must never freeze the picker.
    enum class CaretQuery { Found, NotFound, TimedOut, Unsupported };
    CaretQuery focusedTextRect(QRect &logicalRect, int timeoutMs,
                               int *elapsedMs = nullptr) const;

    // Fire-and-forget at startup: loads the COM/UI Automation machinery so the
    // first real query isn't paying for it and blowing the deadline.
    void warmUpCaretQuery() const;

    // Win32/UIA report physical pixels while Qt works in logical ones; they only
    // coincide at 100% scaling. Everything handed back to Qt goes through this.
    QPoint nativeToLogical(const QPoint &nativePoint) const;

    // Diagnostics: how Qt sees the screens vs how Win32 sees the monitors.
    QStringList describeScreens() const;
    bool insertText(quintptr targetWindow, const QString &text, bool compatibilityPaste);
    // Toggle WS_EX_NOACTIVATE so the picker can float without stealing focus
    // (passive) yet still be activated on demand for search typing. Static so
    // the picker window can also apply it to its popup menus.
    static void setWindowNoActivate(quintptr window, bool noActivate);
    bool activateTarget(quintptr targetWindow) const;
    // Neutralize a held Alt/Win so the focused app does not read its release as
    // the lone-modifier menu gesture (Word's ribbon, Slack's menu bar). Needed
    // wherever the user holds a modifier while the picker — which never takes
    // focus — is on screen, so the raw key still reaches the app underneath.
    void maskModifierMenu() const;
    void trimWorkingSet() const;

private:
    bool sendUnicode(const QString &text) const;
    bool pasteWithClipboard(quintptr targetWindow, const QString &text);
};
