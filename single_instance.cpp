#include "single_instance.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

SingleInstance::SingleInstance(QObject *parent) : QObject(parent) {
    m_pollTimer.setInterval(140);
    connect(&m_pollTimer, &QTimer::timeout, this, [this]() {
#ifdef Q_OS_WIN
        if (m_eventHandle
            && WaitForSingleObject(static_cast<HANDLE>(m_eventHandle), 0) == WAIT_OBJECT_0) {
            emit showRequested();
        }
#endif
    });
}

SingleInstance::~SingleInstance() {
#ifdef Q_OS_WIN
    if (m_eventHandle)
        CloseHandle(static_cast<HANDLE>(m_eventHandle));
#endif
}

bool SingleInstance::startOrNotifyExisting() {
#ifdef Q_OS_WIN
    const wchar_t *eventName = L"Local\\PickMoji-show";
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, eventName);
    if (!eventHandle)
        return true; // Do not prevent startup if the OS event cannot be created.

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        SetEvent(eventHandle);
        CloseHandle(eventHandle);
        return false;
    }

    m_eventHandle = eventHandle;
    m_pollTimer.start();
#endif
    return true;
}

void SingleInstance::release() {
#ifdef Q_OS_WIN
    m_pollTimer.stop();
    if (m_eventHandle) {
        CloseHandle(static_cast<HANDLE>(m_eventHandle));
        m_eventHandle = nullptr;
    }
#endif
}
