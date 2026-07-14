#pragma once

#include <QHash>
#include <QString>
#include <QVector>

struct EmojiEntry {
    QString emoji;
    QString name;
    QString group;
    QString subgroup;
    QString searchable;
    QString variantBase;
    bool isSkinToneVariant = false;
};

class EmojiRepository {
public:
    bool load();

    const QVector<EmojiEntry> &entries() const { return m_entries; }
    const QVector<QString> &groups() const { return m_groups; }
    const QVector<int> &indicesForGroup(const QString &group) const;
    const QVector<int> &skinToneVariantsFor(int entryIndex) const;
    int baseIndexFor(int entryIndex) const;
    int indexOf(const QString &emoji) const;

    static QString displayNameForGroup(const QString &group);
    static QString normalizeSearchText(const QString &text);

    QString unicodeVersion() const { return m_unicodeVersion; }
    QString errorString() const { return m_errorString; }

private:
    QVector<EmojiEntry> m_entries;
    QVector<QString> m_groups;
    QHash<QString, QVector<int>> m_groupIndices;
    QHash<int, QVector<int>> m_skinToneVariants;
    QHash<QString, int> m_emojiIndices;
    QString m_unicodeVersion;
    QString m_errorString;
};
