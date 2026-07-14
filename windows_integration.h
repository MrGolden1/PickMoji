#pragma once

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QKeySequence>
#include <QObject>
#include <QPoint>
#include <QSize>
#include <QString>

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
    // Returns true only when the target exposes a real text caret; lets the
    // caller choose a smarter fallback than the mouse position.
    bool caretPosition(quintptr targetWindow, QPoint &position) const;
    bool insertText(quintptr targetWindow, const QString &text, bool compatibilityPaste);
    // Toggle WS_EX_NOACTIVATE so the picker can float without stealing focus
    // (passive) yet still be activated on demand for search typing.
    void setWindowNoActivate(quintptr window, bool noActivate) const;
    bool activateTarget(quintptr targetWindow) const;
    void trimWorkingSet() const;

private:
    bool sendUnicode(const QString &text) const;
    bool pasteWithClipboard(quintptr targetWindow, const QString &text);
};
