#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>

struct EmojiUsage {
    quint64 totalCount = 0;
    qint64 lastUsed = 0;
    double shortTerm = 0.0;
    double longTerm = 0.0;
    quint64 serial = 0;
};

class UsageStore : public QObject {
    Q_OBJECT

public:
    explicit UsageStore(QObject *parent = nullptr);

    void record(const QString &emoji);
    double score(const QString &emoji, qint64 now = 0) const;
    bool hasUsage(const QString &emoji) const;
    void flush();

signals:
    void usageChanged();

private:
    static double decay(double value, qint64 elapsedSeconds, double halfLifeSeconds);
    void load();
    void save();

    QHash<QString, EmojiUsage> m_usage;
    QTimer m_saveTimer;
    quint64 m_serialCounter = 0;
    bool m_dirty = false;
};
