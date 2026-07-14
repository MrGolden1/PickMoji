#pragma once

#include <QObject>
#include <QTimer>

class SingleInstance final : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QObject *parent = nullptr);
    ~SingleInstance() override;
    bool startOrNotifyExisting();

signals:
    void showRequested();

private:
    QTimer m_pollTimer;
    void *m_eventHandle = nullptr;
};
