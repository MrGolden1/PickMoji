#include "windows_integration.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHash>
#include <QKeyCombination>
#include <QMimeData>
#include <QScreen>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <uiautomation.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
#ifdef Q_OS_WIN
struct NativeHotkey {
    UINT modifiers = 0;
    UINT key = 0;
};

bool toNativeHotkey(const QKeySequence &sequence, NativeHotkey &native, QString &error) {
    if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown) {
        error = QStringLiteral("Choose one key combination.");
        return false;
    }

    const QKeyCombination combination = sequence[0];
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    if (!(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier))) {
        error = QStringLiteral("The shortcut must include Ctrl, Alt, Shift, or Win.");
        return false;
    }
    if (modifiers.testFlag(Qt::ControlModifier)) native.modifiers |= MOD_CONTROL;
    if (modifiers.testFlag(Qt::AltModifier)) native.modifiers |= MOD_ALT;
    if (modifiers.testFlag(Qt::ShiftModifier)) native.modifiers |= MOD_SHIFT;
    if (modifiers.testFlag(Qt::MetaModifier)) native.modifiers |= MOD_WIN;
    native.modifiers |= MOD_NOREPEAT;

    const int key = static_cast<int>(combination.key());
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        native.key = static_cast<UINT>('A' + key - Qt::Key_A);
    else if (key >= Qt::Key_0 && key <= Qt::Key_9)
        native.key = static_cast<UINT>('0' + key - Qt::Key_0);
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        native.key = static_cast<UINT>(VK_F1 + key - Qt::Key_F1);
    else {
        switch (key) {
        case Qt::Key_Period: native.key = VK_OEM_PERIOD; break;
        case Qt::Key_Comma: native.key = VK_OEM_COMMA; break;
        case Qt::Key_Semicolon: native.key = VK_OEM_1; break;
        case Qt::Key_Slash: native.key = VK_OEM_2; break;
        case Qt::Key_QuoteLeft: native.key = VK_OEM_3; break;
        case Qt::Key_BracketLeft: native.key = VK_OEM_4; break;
        case Qt::Key_Backslash: native.key = VK_OEM_5; break;
        case Qt::Key_BracketRight: native.key = VK_OEM_6; break;
        case Qt::Key_Apostrophe: native.key = VK_OEM_7; break;
        case Qt::Key_Minus: native.key = VK_OEM_MINUS; break;
        case Qt::Key_Equal: native.key = VK_OEM_PLUS; break;
        case Qt::Key_Space: native.key = VK_SPACE; break;
        case Qt::Key_Tab: native.key = VK_TAB; break;
        case Qt::Key_Return:
        case Qt::Key_Enter: native.key = VK_RETURN; break;
        case Qt::Key_Escape: native.key = VK_ESCAPE; break;
        case Qt::Key_Insert: native.key = VK_INSERT; break;
        case Qt::Key_Delete: native.key = VK_DELETE; break;
        case Qt::Key_Home: native.key = VK_HOME; break;
        case Qt::Key_End: native.key = VK_END; break;
        case Qt::Key_PageUp: native.key = VK_PRIOR; break;
        case Qt::Key_PageDown: native.key = VK_NEXT; break;
        case Qt::Key_Left: native.key = VK_LEFT; break;
        case Qt::Key_Right: native.key = VK_RIGHT; break;
        case Qt::Key_Up: native.key = VK_UP; break;
        case Qt::Key_Down: native.key = VK_DOWN; break;
        default:
            error = QStringLiteral("That key is not supported as a global shortcut.");
            return false;
        }
    }
    return true;
}

// Shared state for an off-thread UI Automation query. Held by shared_ptr so the
// detached worker can safely finish writing even after we have given up waiting.
struct FocusRectQuery {
    std::mutex mutex;
    std::condition_variable done;
    bool completed = false;
    bool found = false;
    QRect rect; // native/physical screen pixels
};

void runFocusRectQuery(std::shared_ptr<FocusRectQuery> state) {
    QRect result;
    bool found = false;

    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        IUIAutomation *automation = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&automation)))
            && automation) {
            // The picker never takes focus, so the globally focused element is
            // exactly the control the user is typing in.
            IUIAutomationElement *focused = nullptr;
            if (SUCCEEDED(automation->GetFocusedElement(&focused)) && focused) {
                IUIAutomationTextPattern *textPattern = nullptr;
                if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPatternId,
                                                           IID_PPV_ARGS(&textPattern)))
                    && textPattern) {
                    IUIAutomationTextRangeArray *ranges = nullptr;
                    if (SUCCEEDED(textPattern->GetSelection(&ranges)) && ranges) {
                        int count = 0;
                        ranges->get_Length(&count);
                        IUIAutomationTextRange *range = nullptr;
                        if (count > 0 && SUCCEEDED(ranges->GetElement(0, &range)) && range) {
                            SAFEARRAY *bounds = nullptr;
                            if (SUCCEEDED(range->GetBoundingRectangles(&bounds)) && bounds) {
                                LONG lower = 0;
                                LONG upper = -1;
                                double *values = nullptr;
                                // Groups of four doubles: left, top, width, height.
                                if (SUCCEEDED(SafeArrayGetLBound(bounds, 1, &lower))
                                    && SUCCEEDED(SafeArrayGetUBound(bounds, 1, &upper))
                                    && upper - lower + 1 >= 4
                                    && SUCCEEDED(SafeArrayAccessData(
                                           bounds, reinterpret_cast<void **>(&values)))) {
                                    // A collapsed caret is zero-width; keep it visible.
                                    result = QRect(qRound(values[0]), qRound(values[1]),
                                                   std::max(1, qRound(values[2])),
                                                   qRound(values[3]));
                                    found = result.height() > 0;
                                    SafeArrayUnaccessData(bounds);
                                }
                                SafeArrayDestroy(bounds);
                            }
                            range->Release();
                        }
                        if (ranges)
                            ranges->Release();
                    }
                    textPattern->Release();
                }

                // No text pattern (or an empty selection): fall back to the
                // focused control's own box, which is still a good anchor.
                if (!found) {
                    RECT box = {};
                    if (SUCCEEDED(focused->get_CurrentBoundingRectangle(&box))) {
                        result = QRect(QPoint(box.left, box.top),
                                       QPoint(box.right - 1, box.bottom - 1));
                        found = result.width() > 0 && result.height() > 0;
                    }
                }
                focused->Release();
            }
            automation->Release();
        }
        CoUninitialize();
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->rect = result;
        state->found = found;
        state->completed = true;
    }
    state->done.notify_all();
}
#endif
} // namespace

GlobalHotkey::GlobalHotkey(QObject *parent) : QObject(parent) {
    QCoreApplication::instance()->installNativeEventFilter(this);
}

GlobalHotkey::~GlobalHotkey() {
#ifdef Q_OS_WIN
    if (m_registered)
        UnregisterHotKey(nullptr, HOTKEY_ID);
    if (m_dismissRegistered)
        UnregisterHotKey(nullptr, DISMISS_HOTKEY_ID);
#endif
    if (QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(this);
}

QKeySequence GlobalHotkey::defaultShortcut() {
    return QKeySequence(QKeyCombination(Qt::AltModifier, Qt::Key_Period));
}

bool GlobalHotkey::registerShortcut(const QKeySequence &shortcut) {
#ifdef Q_OS_WIN
    NativeHotkey requested;
    QString validationError;
    if (!toNativeHotkey(shortcut, requested, validationError)) {
        m_errorString = validationError;
        return false;
    }

    const QKeySequence previous = m_shortcut;
    const bool hadPrevious = m_registered;
    if (m_registered) {
        UnregisterHotKey(nullptr, HOTKEY_ID);
        m_registered = false;
    }

    if (RegisterHotKey(nullptr, HOTKEY_ID, requested.modifiers, requested.key)) {
        m_registered = true;
        m_shortcut = shortcut;
        m_errorString.clear();
        return true;
    }

    if (hadPrevious) {
        NativeHotkey oldNative;
        QString ignored;
        if (toNativeHotkey(previous, oldNative, ignored))
            m_registered = RegisterHotKey(nullptr, HOTKEY_ID, oldNative.modifiers, oldNative.key);
    }
    m_shortcut = previous;
    m_errorString = QStringLiteral("%1 is already used by Windows or another application.")
        .arg(shortcut.toString(QKeySequence::NativeText));
    return false;
#else
    Q_UNUSED(shortcut);
    m_errorString = QStringLiteral("Global shortcuts are currently implemented for Windows only.");
    return false;
#endif
}

bool GlobalHotkey::registerDismissKey() {
#ifdef Q_OS_WIN
    if (m_dismissRegistered)
        return true;
    m_dismissRegistered = RegisterHotKey(nullptr, DISMISS_HOTKEY_ID, MOD_NOREPEAT, VK_ESCAPE);
    return m_dismissRegistered;
#else
    return false;
#endif
}

void GlobalHotkey::unregisterDismissKey() {
#ifdef Q_OS_WIN
    if (m_dismissRegistered) {
        UnregisterHotKey(nullptr, DISMISS_HOTKEY_ID);
        m_dismissRegistered = false;
    }
#endif
}

bool GlobalHotkey::nativeEventFilter(const QByteArray &, void *message, qintptr *) {
#ifdef Q_OS_WIN
    const MSG *nativeMessage = static_cast<const MSG *>(message);
    if (nativeMessage && nativeMessage->message == WM_HOTKEY) {
        const int hotkeyId = static_cast<int>(nativeMessage->wParam);
        if (hotkeyId == HOTKEY_ID) {
            emit activated();
            return true;
        }
        if (hotkeyId == DISMISS_HOTKEY_ID) {
            emit dismissPressed();
            return true;
        }
    }
#else
    Q_UNUSED(message);
#endif
    return false;
}

WindowsIntegration::WindowsIntegration(QObject *parent) : QObject(parent) {}

void WindowsIntegration::trimWorkingSet() const {
#ifdef Q_OS_WIN
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
#endif
}

quintptr WindowsIntegration::foregroundWindow() const {
#ifdef Q_OS_WIN
    return reinterpret_cast<quintptr>(GetForegroundWindow());
#else
    return 0;
#endif
}

bool WindowsIntegration::isForeground(quintptr window) const {
#ifdef Q_OS_WIN
    return window != 0 && GetForegroundWindow() == reinterpret_cast<HWND>(window);
#else
    Q_UNUSED(window);
    return false;
#endif
}

bool WindowsIntegration::isPointerButtonDown() const {
#ifdef Q_OS_WIN
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0
        || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
#else
    return false;
#endif
}

void WindowsIntegration::setWindowNoActivate(quintptr window, bool noActivate) const {
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!hwnd)
        return;
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (noActivate)
        exStyle |= WS_EX_NOACTIVATE;
    else
        exStyle &= ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
#else
    Q_UNUSED(window);
    Q_UNUSED(noActivate);
#endif
}

bool WindowsIntegration::isUsableTarget(quintptr window, quintptr pickerWindow) const {
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(window);
    if (!hwnd || window == pickerWindow || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
        return false;

    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, 128);
    const QString windowClass = QString::fromWCharArray(className);
    return windowClass != QLatin1String("Shell_TrayWnd")
        && windowClass != QLatin1String("Shell_SecondaryTrayWnd")
        && windowClass != QLatin1String("NotifyIconOverflowWindow")
        && windowClass != QLatin1String("Progman")
        && windowClass != QLatin1String("WorkerW");
#else
    Q_UNUSED(window);
    Q_UNUSED(pickerWindow);
    return false;
#endif
}

QPoint WindowsIntegration::caretOrCursorPosition(quintptr targetWindow) const {
#ifdef Q_OS_WIN
    const HWND target = reinterpret_cast<HWND>(targetWindow);
    if (target) {
        const DWORD threadId = GetWindowThreadProcessId(target, nullptr);
        GUITHREADINFO information = {};
        information.cbSize = sizeof(information);
        if (threadId && GetGUIThreadInfo(threadId, &information) && information.hwndCaret) {
            POINT point = {information.rcCaret.left, information.rcCaret.bottom};
            if (ClientToScreen(information.hwndCaret, &point))
                return QPoint(point.x, point.y);
        }
    }
    POINT cursor = {};
    if (GetCursorPos(&cursor))
        return QPoint(cursor.x, cursor.y);
#else
    Q_UNUSED(targetWindow);
#endif
    return QCursor::pos();
}

bool WindowsIntegration::caretRect(quintptr targetWindow, QRect &logicalRect) const {
#ifdef Q_OS_WIN
    const HWND target = reinterpret_cast<HWND>(targetWindow);
    if (!target)
        return false;
    const DWORD threadId = GetWindowThreadProcessId(target, nullptr);
    GUITHREADINFO information = {};
    information.cbSize = sizeof(information);
    if (threadId && GetGUIThreadInfo(threadId, &information) && information.hwndCaret
        && information.rcCaret.bottom > information.rcCaret.top) { // reject hidden 0-height carets
        POINT topLeft = {information.rcCaret.left, information.rcCaret.top};
        POINT bottomRight = {information.rcCaret.right, information.rcCaret.bottom};
        if (ClientToScreen(information.hwndCaret, &topLeft)
            && ClientToScreen(information.hwndCaret, &bottomRight)) {
            logicalRect = QRect(nativeToLogical(QPoint(topLeft.x, topLeft.y)),
                                nativeToLogical(QPoint(bottomRight.x, bottomRight.y)));
            // A caret is zero-width; give it substance so it can be avoided.
            if (logicalRect.width() < 2)
                logicalRect.setWidth(2);
            return logicalRect.height() > 0;
        }
    }
    return false;
#else
    Q_UNUSED(targetWindow);
    Q_UNUSED(logicalRect);
    return false;
#endif
}

QPoint WindowsIntegration::nativeToLogical(const QPoint &nativePoint) const {
#ifdef Q_OS_WIN
    const POINT point = {nativePoint.x(), nativePoint.y()};
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW information = {};
    information.cbSize = sizeof(information);
    if (monitor && GetMonitorInfoW(monitor, &information)) {
        // QScreen::name() on Windows is the GDI device name (\\.\DISPLAY1),
        // which is exactly what MONITORINFOEX reports — a reliable pairing.
        const QString deviceName = QString::fromWCharArray(information.szDevice);
        const QList<QScreen *> screens = QGuiApplication::screens();
        for (QScreen *screen : screens) {
            if (screen->name() != deviceName)
                continue;
            const qreal ratio = screen->devicePixelRatio();
            if (ratio <= 0.0)
                break;
            const QPoint offset(qRound((nativePoint.x() - information.rcMonitor.left) / ratio),
                                qRound((nativePoint.y() - information.rcMonitor.top) / ratio));
            return screen->geometry().topLeft() + offset;
        }
    }
#endif
    return nativePoint;
}

void WindowsIntegration::warmUpCaretQuery() const {
#ifdef Q_OS_WIN
    // No one waits on this: it exists purely to pull UIAutomationCore and the COM
    // proxies into the process ahead of the first user-visible query.
    std::thread(runFocusRectQuery, std::make_shared<FocusRectQuery>()).detach();
#endif
}

WindowsIntegration::CaretQuery WindowsIntegration::focusedTextRect(QRect &logicalRect,
                                                                  int timeoutMs,
                                                                  int *elapsedMs) const {
#ifdef Q_OS_WIN
    QElapsedTimer timer;
    timer.start();
    auto state = std::make_shared<FocusRectQuery>();
    // Detached on purpose: UI Automation is cross-process COM and can block on a
    // busy target. We refuse to wait longer than the deadline; the worker keeps
    // the shared state alive and simply finishes into a result nobody reads.
    std::thread(runFocusRectQuery, state).detach();

    QRect native;
    bool timedOut = false;
    bool found = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        timedOut = !state->done.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                         [&state] { return state->completed; });
        if (!timedOut) {
            found = state->found;
            native = state->rect;
        }
    }
    if (elapsedMs)
        *elapsedMs = static_cast<int>(timer.elapsed());

    if (timedOut)
        return CaretQuery::TimedOut;
    if (!found)
        return CaretQuery::NotFound;

    const QPoint topLeft = nativeToLogical(native.topLeft());
    const QPoint bottomRight = nativeToLogical(native.bottomRight());
    logicalRect = QRect(topLeft, bottomRight);
    return logicalRect.height() > 0 ? CaretQuery::Found : CaretQuery::NotFound;
#else
    Q_UNUSED(logicalRect);
    Q_UNUSED(timeoutMs);
    Q_UNUSED(elapsedMs);
    return CaretQuery::Unsupported;
#endif
}

bool WindowsIntegration::activateTarget(quintptr targetWindow) const {
#ifdef Q_OS_WIN
    const HWND target = reinterpret_cast<HWND>(targetWindow);
    if (!target || !IsWindow(target))
        return false;
    if (IsIconic(target))
        ShowWindow(target, SW_RESTORE);

    const HWND foreground = GetForegroundWindow();
    const DWORD currentThread = GetCurrentThreadId();
    const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attached = foregroundThread && foregroundThread != currentThread
        && AttachThreadInput(currentThread, foregroundThread, TRUE);

    BringWindowToTop(target);
    SetForegroundWindow(target);
    SetActiveWindow(target);
    const bool activated = GetForegroundWindow() == target;

    if (attached)
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    return activated;
#else
    Q_UNUSED(targetWindow);
    return false;
#endif
}

bool WindowsIntegration::sendUnicode(const QString &text) const {
#ifdef Q_OS_WIN
    if (text.isEmpty())
        return false;
    std::vector<INPUT> inputs;
    inputs.reserve(static_cast<size_t>(text.size()) * 2);
    for (const QChar character : text) {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = character.unicode();
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);

        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }
    const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    return sent == inputs.size();
#else
    Q_UNUSED(text);
    return false;
#endif
}

bool WindowsIntegration::pasteWithClipboard(quintptr targetWindow, const QString &text) {
#ifdef Q_OS_WIN
    QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *current = clipboard->mimeData();
    QHash<QString, QByteArray> restoreData;
    if (current) {
        for (const QString &format : current->formats())
            restoreData.insert(format, current->data(format));
    }

    clipboard->setText(text);
    // The target usually still holds the foreground in the non-activating model,
    // so only force a foreground switch (and its title-bar flash) when required.
    if (!isForeground(targetWindow) && !activateTarget(targetWindow)) {
        return false;
    }

    INPUT inputs[4] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[2] = inputs[1];
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3] = inputs[0];
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    const bool sent = SendInput(4, inputs, sizeof(INPUT)) == 4;

    QTimer::singleShot(900, this, [restoreData, text]() {
        QClipboard *activeClipboard = QGuiApplication::clipboard();
        if (activeClipboard->text() == text) {
            auto *restore = new QMimeData;
            for (auto it = restoreData.cbegin(); it != restoreData.cend(); ++it)
                restore->setData(it.key(), it.value());
            activeClipboard->setMimeData(restore);
        }
    });
    return sent;
#else
    Q_UNUSED(targetWindow);
    Q_UNUSED(text);
    return false;
#endif
}

bool WindowsIntegration::insertText(quintptr targetWindow, const QString &text, bool compatibilityPaste) {
    if (!isUsableTarget(targetWindow))
        return false;
    // Keyboard injection is ideal for a single Unicode scalar. Composite emoji
    // must be pasted atomically; some controls reorder ZWJ/variation inputs when
    // their UTF-16 units arrive as separate WM_CHAR messages.
    const bool compositeSequence = text.toUcs4().size() != 1;
    if (compatibilityPaste || compositeSequence)
        return pasteWithClipboard(targetWindow, text);
    // Direct Unicode goes to the foreground window; only steal focus (causing a
    // visible blink) when the target is not already foregrounded.
    if (!isForeground(targetWindow) && !activateTarget(targetWindow))
        return false;
    return sendUnicode(text);
}
