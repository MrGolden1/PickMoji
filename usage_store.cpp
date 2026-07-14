#include "usage_store.h"

#include <QDateTime>
#include <QSettings>

#include <algorithm>
#include <cmath>

namespace {
constexpr double SHORT_HALF_LIFE = 3.0 * 24.0 * 60.0 * 60.0;
constexpr double LONG_HALF_LIFE = 30.0 * 24.0 * 60.0 * 60.0;
constexpr double RECENCY_HALF_LIFE = 18.0 * 60.0 * 60.0;
}

UsageStore::UsageStore(QObject *parent) : QObject(parent) {
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1800);
    connect(&m_saveTimer, &QTimer::timeout, this, &UsageStore::save);
    load();
}

double UsageStore::decay(double value, qint64 elapsedSeconds, double halfLifeSeconds) {
    if (value <= 0.0 || elapsedSeconds <= 0)
        return value;
    return value * std::pow(0.5, static_cast<double>(elapsedSeconds) / halfLifeSeconds);
}

void UsageStore::record(const QString &emoji) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    EmojiUsage &usage = m_usage[emoji];
    if (usage.lastUsed > 0) {
        const qint64 elapsed = std::max<qint64>(0, now - usage.lastUsed);
        usage.shortTerm = decay(usage.shortTerm, elapsed, SHORT_HALF_LIFE);
        usage.longTerm = decay(usage.longTerm, elapsed, LONG_HALF_LIFE);
    }

    usage.shortTerm += 1.0;
    usage.longTerm += 1.0;
    usage.totalCount += 1;
    usage.lastUsed = now;
    usage.serial = ++m_serialCounter;
    m_dirty = true;
    m_saveTimer.start();
    emit usageChanged();
}

void UsageStore::remove(const QString &emoji) {
    if (m_usage.remove(emoji) == 0)
        return;
    m_dirty = true;
    m_saveTimer.start();
    emit usageChanged();
}

double UsageStore::score(const QString &emoji, qint64 now) const {
    const auto it = m_usage.constFind(emoji);
    if (it == m_usage.cend() || it->lastUsed <= 0)
        return 0.0;

    if (now <= 0)
        now = QDateTime::currentSecsSinceEpoch();
    const qint64 age = std::max<qint64>(0, now - it->lastUsed);
    const double recency = std::pow(0.5, static_cast<double>(age) / RECENCY_HALF_LIFE);
    const double shortTerm = decay(it->shortTerm, age, SHORT_HALF_LIFE);
    const double longTerm = decay(it->longTerm, age, LONG_HALF_LIFE);

    // Three time scales keep new choices responsive without letting one-off
    // selections permanently displace genuinely frequent emoji.
    return 6.0 * recency
        + 2.8 * std::log1p(shortTerm)
        + 1.35 * std::log1p(longTerm)
        + 0.35 * std::log1p(static_cast<double>(it->totalCount))
        + static_cast<double>(it->serial % 1000000) * 1e-9;
}

bool UsageStore::hasUsage(const QString &emoji) const {
    const auto it = m_usage.constFind(emoji);
    return it != m_usage.cend() && it->totalCount > 0;
}

void UsageStore::load() {
    QSettings settings("PickMoji", "PickMoji");
    m_serialCounter = settings.value("usageSerial", 0).toULongLong();
    const int size = settings.beginReadArray("usage");
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        const QString emoji = settings.value("emoji").toString();
        if (emoji.isEmpty())
            continue;

        EmojiUsage usage;
        usage.totalCount = settings.value("totalCount", settings.value("count", 0)).toULongLong();
        usage.lastUsed = settings.value("lastUsed", 0).toLongLong();
        usage.shortTerm = settings.value("shortTerm", static_cast<double>(usage.totalCount)).toDouble();
        usage.longTerm = settings.value("longTerm", static_cast<double>(usage.totalCount)).toDouble();
        const quint64 fallbackSerial = m_serialCounter + 1;
        usage.serial = settings.value("serial", fallbackSerial).toULongLong();
        m_serialCounter = std::max(m_serialCounter, usage.serial);
        m_usage.insert(emoji, usage);
    }
    settings.endArray();
}

void UsageStore::save() {
    if (!m_dirty)
        return;

    QSettings settings("PickMoji", "PickMoji");
    settings.remove("usage");
    settings.beginWriteArray("usage", m_usage.size());
    int index = 0;
    for (auto it = m_usage.cbegin(); it != m_usage.cend(); ++it, ++index) {
        settings.setArrayIndex(index);
        settings.setValue("emoji", it.key());
        settings.setValue("totalCount", static_cast<qulonglong>(it->totalCount));
        settings.setValue("lastUsed", it->lastUsed);
        settings.setValue("shortTerm", it->shortTerm);
        settings.setValue("longTerm", it->longTerm);
        settings.setValue("serial", static_cast<qulonglong>(it->serial));
    }
    settings.endArray();
    settings.setValue("usageSerial", static_cast<qulonglong>(m_serialCounter));
    settings.sync();
    m_dirty = false;
}

void UsageStore::flush() {
    m_saveTimer.stop();
    save();
}
