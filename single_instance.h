#pragma once

#include <QObject>
#include <QTimer>

class SingleInstance final : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QObject *parent = nullptr);
    ~SingleInstance() override;
    bool startOrNotifyExisting();
    // Give up ownership of the single-instance slot so a replacement process
    // (a self-update relaunch) can claim it without racing this one's exit.
    void release();

signals:
    void showRequested();

private:
    QTimer m_pollTimer;
    void *m_eventHandle = nullptr;
};
