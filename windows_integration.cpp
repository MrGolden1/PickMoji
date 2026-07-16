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
#include <limits>
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

// RegisterHotKey consumes the hotkey's own key, but the app underneath still
// sees the bare Alt/Win press and release around it — and a lone Alt release is
// the menu/ribbon activation gesture in Word, Slack and friends (which is why
// the hotkey seemed to trigger extra actions there). Injecting a no-op key
// (0xFF, assigned to nothing; the same mask AutoHotkey uses) while the modifier
// is still down makes the apps read it as a combo instead, so no menu pops.
void maskModifierMenuActivation() {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = 0xFF;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 0xFF;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

// SetForegroundWindow returning is not the same as the app being ready for
// input: it still has to move keyboard focus to its focused control (Word's
// document, say). Keystrokes injected inside that gap are silently dropped, so
// after a forced activation give the transition a moment to finish.
void waitForInputSettle(HWND target) {
    for (int attempt = 0; attempt < 10 && GetForegroundWindow() != target; ++attempt)
        Sleep(15);
    Sleep(40);
}

// Injected input inherits whatever modifiers the user is physically holding, and
// the skin-tone gesture means Alt is still down at the moment of insertion. That
// turns the paste into Ctrl+Alt+V (a different command — Word does nothing but
// flash) and direct Unicode into WM_SYSCHAR (a menu accelerator, not text). So
// release every held modifier first. The user's own key-up afterwards is a
// harmless duplicate.
void releaseHeldModifiers() {
    static const WORD MODIFIER_KEYS[] = {
        VK_LMENU, VK_RMENU, VK_LCONTROL, VK_RCONTROL,
        VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN,
    };
    std::vector<INPUT> inputs;
    for (WORD key : MODIFIER_KEYS) {
        if (!(GetAsyncKeyState(key) & 0x8000))
            continue;
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
    if (!inputs.empty())
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
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

// Pull a bounding rectangle (physical pixels) out of a UIA text range. The
// SAFEARRAY holds groups of four doubles: left, top, width, height.
bool rectFromTextRange(IUIAutomationTextRange *range, QRect &out) {
    SAFEARRAY *bounds = nullptr;
    if (FAILED(range->GetBoundingRectangles(&bounds)) || !bounds)
        return false;
    bool ok = false;
    LONG lower = 0;
    LONG upper = -1;
    double *values = nullptr;
    if (SUCCEEDED(SafeArrayGetLBound(bounds, 1, &lower))
        && SUCCEEDED(SafeArrayGetUBound(bounds, 1, &upper))
        && upper - lower + 1 >= 4
        && SUCCEEDED(SafeArrayAccessData(bounds, reinterpret_cast<void **>(&values)))) {
        // A collapsed caret is zero-width; keep it at least 1px so it survives.
        out = QRect(qRound(values[0]), qRound(values[1]),
                    std::max(1, qRound(values[2])), qRound(values[3]));
        ok = out.height() > 0;
        SafeArrayUnaccessData(bounds);
    }
    SafeArrayDestroy(bounds);
    return ok;
}

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
                            // Is the selection collapsed (a caret) or a real
                            // highlighted span?
                            int endpointCompare = 1;
                            const bool collapsed = SUCCEEDED(range->CompareEndpoints(
                                    TextPatternRangeEndpoint_Start, range,
                                    TextPatternRangeEndpoint_End, &endpointCompare))
                                && endpointCompare == 0;

                            bool haveRect = false;
                            if (collapsed) {
                                // A caret has no width of its own, and apps report
                                // its rectangle inconsistently — some give nothing,
                                // some give the whole line (VS Code). Neither
                                // locates the caret, so ignore the range's own rect
                                // and instead grow a clone by one character: the
                                // char to the right of the caret (or the left one
                                // at end of text). That character's box has an edge
                                // at the caret's true x.
                                IUIAutomationTextRange *caret = nullptr;
                                if (SUCCEEDED(range->Clone(&caret)) && caret) {
                                    int moved = 0;
                                    if ((SUCCEEDED(caret->MoveEndpointByUnit(
                                             TextPatternRangeEndpoint_End,
                                             TextUnit_Character, 1, &moved)) && moved != 0)
                                        || (SUCCEEDED(caret->MoveEndpointByUnit(
                                                TextPatternRangeEndpoint_Start,
                                                TextUnit_Character, -1, &moved)) && moved != 0)) {
                                        haveRect = rectFromTextRange(caret, result);
                                    }
                                    caret->Release();
                                }
                            }
                            // A real selection, or the one-character trick failed:
                            // fall back to the range's own bounding rectangle.
                            if (!haveRect)
                                haveRect = rectFromTextRange(range, result);
                            found = haveRect && result.height() > 0;
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
            const Qt::KeyboardModifiers modifiers = m_shortcut.isEmpty()
                ? Qt::KeyboardModifiers(Qt::NoModifier)
                : m_shortcut[0].keyboardModifiers();
            if (modifiers & (Qt::AltModifier | Qt::MetaModifier))
                maskModifierMenuActivation();
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

void WindowsIntegration::maskModifierMenu() const {
#ifdef Q_OS_WIN
    maskModifierMenuActivation();
#endif
}

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

void WindowsIntegration::setWindowNoActivate(quintptr window, bool noActivate) {
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
    // Pair the point to a screen by GEOMETRY, not by name. QScreen::name() on
    // Windows is the monitor's model ("VG27AQ3A"), not the GDI device name
    // ("\\.\DISPLAY5"), so matching on it silently never hits — which left every
    // rect in physical pixels while the rest of the app worked in logical ones.
    //
    // Qt keeps each screen's origin unscaled and scales only its size, so a
    // screen's native rect is (logical topLeft, logical size x dpr).
    const QList<QScreen *> screens = QGuiApplication::screens();

    const auto nativeRectOf = [](const QScreen *screen) {
        const QRect logical = screen->geometry();
        const qreal ratio = screen->devicePixelRatio();
        return QRect(logical.topLeft(), QSize(qRound(logical.width() * ratio),
                                              qRound(logical.height() * ratio)));
    };

    QScreen *match = nullptr;
    for (QScreen *screen : screens) {
        if (screen->devicePixelRatio() > 0.0 && nativeRectOf(screen).contains(nativePoint)) {
            match = screen;
            break;
        }
    }
    if (!match) {
        // Off-screen or stale coordinates: fall back to the nearest screen.
        int bestDistance = std::numeric_limits<int>::max();
        for (QScreen *screen : screens) {
            if (screen->devicePixelRatio() <= 0.0)
                continue;
            const int distance = (nativeRectOf(screen).center() - nativePoint).manhattanLength();
            if (distance < bestDistance) {
                bestDistance = distance;
                match = screen;
            }
        }
    }
    if (!match)
        return nativePoint;

    const qreal ratio = match->devicePixelRatio();
    const QPoint origin = match->geometry().topLeft();
    return origin + QPoint(qRound((nativePoint.x() - origin.x()) / ratio),
                           qRound((nativePoint.y() - origin.y()) / ratio));
}

QStringList WindowsIntegration::describeScreens() const {
    QStringList lines;
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        const QRect geometry = screen->geometry();
        lines << QStringLiteral("qtScreen name='%1' geo=(%2,%3 %4x%5) dpr=%6")
                     .arg(screen->name())
                     .arg(geometry.x()).arg(geometry.y())
                     .arg(geometry.width()).arg(geometry.height())
                     .arg(screen->devicePixelRatio());
    }
#ifdef Q_OS_WIN
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
        auto *out = reinterpret_cast<QStringList *>(param);
        MONITORINFOEXW information = {};
        information.cbSize = sizeof(information);
        if (GetMonitorInfoW(monitor, &information)) {
            *out << QStringLiteral("win32Monitor name='%1' rc=(%2,%3 %4x%5)")
                        .arg(QString::fromWCharArray(information.szDevice))
                        .arg(information.rcMonitor.left).arg(information.rcMonitor.top)
                        .arg(information.rcMonitor.right - information.rcMonitor.left)
                        .arg(information.rcMonitor.bottom - information.rcMonitor.top);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&lines));
#endif
    return lines;
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
    if (!isForeground(targetWindow)) {
        if (!activateTarget(targetWindow))
            return false;
        waitForInputSettle(reinterpret_cast<HWND>(targetWindow));
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
#ifdef Q_OS_WIN
    // Alt is typically still held here (the skin-tone gesture), and it would
    // ride along on whatever we inject next.
    releaseHeldModifiers();
#endif
    // Keyboard injection is ideal for a single Unicode scalar. Composite emoji
    // must be pasted atomically; some controls reorder ZWJ/variation inputs when
    // their UTF-16 units arrive as separate WM_CHAR messages.
    const bool compositeSequence = text.toUcs4().size() != 1;
    if (compatibilityPaste || compositeSequence)
        return pasteWithClipboard(targetWindow, text);
    // Direct Unicode goes to the foreground window; only steal focus (causing a
    // visible blink) when the target is not already foregrounded.
    if (!isForeground(targetWindow)) {
        if (!activateTarget(targetWindow))
            return false;
#ifdef Q_OS_WIN
        waitForInputSettle(reinterpret_cast<HWND>(targetWindow));
#endif
    }
    return sendUnicode(text);
}
