#include "emoji_repository.h"

#include "emoji_keywords.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

#include <utility>

namespace {
const QVector<int> EMPTY_INDICES;

QString groupSearchAliases(const QString &group) {
    static const QHash<QString, QString> aliases = {
        {"Smileys & Emotion", "emoji smiley emotion face احساس صورت لبخند ایموجی"},
        {"People & Body", "people body hand person مردم آدم بدن دست انسان"},
        {"Animals & Nature", "animal nature plant حیوان طبیعت گیاه"},
        {"Food & Drink", "food drink meal غذا نوشیدنی خوراکی"},
        {"Travel & Places", "travel place transport سفر مکان حمل نقل"},
        {"Activities", "activity sport game فعالیت ورزش بازی"},
        {"Objects", "object thing tool اشیا وسیله ابزار"},
        {"Symbols", "symbol sign علامت نماد"},
        {"Flags", "flag country پرچم کشور"},
    };
    return aliases.value(group);
}

// Keyword-file keys match with variation selectors stripped, so authors don't
// have to know whether the catalogue spells an emoji "❤" or "❤️".
QString keywordKey(const QString &emoji) {
    QString result;
    for (uint point : emoji.toUcs4()) {
        if (point == 0xFE0F || point == 0xFE0E)
            continue;
        const char32_t scalar = static_cast<char32_t>(point);
        result.append(QString::fromUcs4(&scalar, 1));
    }
    return result;
}

// Search keywords are extensible without a rebuild: every *.json file in
// <exe dir>/keywords (packs shipped with the app) and <AppData>/PickMoji/keywords
// (the user's own) is merged into the search index at startup. Format:
//   { "language": "de", "keywords": { "😀": "lachen glücklich",
//                                     "❤️": ["herz", "liebe"] } }
// A bare top-level map without the "keywords" wrapper is accepted too. Values
// only ever *add* search terms, so a bad pack can't break built-in search.
QHash<QString, QString> loadExternalKeywords() {
    QHash<QString, QString> merged;

    QStringList directories;
    directories << QCoreApplication::applicationDirPath() + QStringLiteral("/keywords");
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!appData.isEmpty())
        directories << appData + QStringLiteral("/keywords");

    for (const QString &directoryPath : std::as_const(directories)) {
        const QDir directory(directoryPath);
        const QStringList files =
            directory.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QString &fileName : files) {
            QFile file(directory.filePath(fileName));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
            if (!document.isObject())
                continue;
            const QJsonObject root = document.object();
            const QJsonObject map = root.contains(QLatin1String("keywords"))
                ? root.value(QLatin1String("keywords")).toObject() : root;
            for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
                QString words;
                if (it.value().isArray()) {
                    QStringList list;
                    for (const QJsonValue &value : it.value().toArray())
                        list << value.toString();
                    words = list.join(QLatin1Char(' '));
                } else {
                    words = it.value().toString();
                }
                if (words.trimmed().isEmpty())
                    continue;
                QString &slot = merged[keywordKey(it.key())];
                if (!slot.isEmpty())
                    slot += QLatin1Char(' ');
                slot += words;
            }
        }
    }
    return merged;
}

// Tests the same font family EmojiCanvas actually paints with, so a "yes" here
// means it will really render and not fall back to a placeholder box. Joiners,
// variation selectors and skin-tone modifiers are old, universally-supported
// code points; skipping them means a brand-new *base* glyph is what gets
// caught, rather than a false negative on an otherwise-fine ZWJ sequence.
bool glyphRenders(const QString &emoji) {
    static const QFontMetrics metrics{QFont(QStringLiteral("Segoe UI Emoji"))};
    for (uint point : emoji.toUcs4()) {
        if (point == 0x200D || point == 0xFE0E || point == 0xFE0F)
            continue;
        if (point >= 0x1F3FB && point <= 0x1F3FF)
            continue;
        if (!metrics.inFontUcs4(point))
            return false;
    }
    return true;
}

QString skinToneFamilyKey(const QString &emoji, bool *removedTone) {
    QString result;
    bool found = false;
    for (uint point : emoji.toUcs4()) {
        if (point >= 0x1F3FB && point <= 0x1F3FF) {
            found = true;
            continue;
        }
        if (point == 0xFE0F)
            continue; // Fully-qualified bases and toned variants differ here.
        const char32_t scalar = static_cast<char32_t>(point);
        result.append(QString::fromUcs4(&scalar, 1));
    }
    if (removedTone)
        *removedTone = found;

    // Unicode keeps legacy single-codepoint bases for these multi-person
    // emoji, while mixed-tone variants use newer component sequences.
    static const QHash<QString, QString> legacyFamilyAliases = {
        {QStringLiteral("🫱‍🫲"), QStringLiteral("🤝")},
        {QStringLiteral("🧑‍🐰‍🧑"), QStringLiteral("👯")},
        {QStringLiteral("👨‍🐰‍👨"), QStringLiteral("👯‍♂")},
        {QStringLiteral("👩‍🐰‍👩"), QStringLiteral("👯‍♀")},
        {QStringLiteral("🧑‍🫯‍🧑"), QStringLiteral("🤼")},
        {QStringLiteral("👨‍🫯‍👨"), QStringLiteral("🤼‍♂")},
        {QStringLiteral("👩‍🫯‍👩"), QStringLiteral("🤼‍♀")},
        {QStringLiteral("👩‍🤝‍👩"), QStringLiteral("👭")},
        {QStringLiteral("👩‍🤝‍👨"), QStringLiteral("👫")},
        {QStringLiteral("👨‍🤝‍👨"), QStringLiteral("👬")},
        {QStringLiteral("🧑‍❤‍💋‍🧑"), QStringLiteral("💏")},
        {QStringLiteral("🧑‍❤‍🧑"), QStringLiteral("💑")},
    };
    return legacyFamilyAliases.value(result, result);
}
} // namespace

bool EmojiRepository::load() {
    m_entries.clear();
    m_groups.clear();
    m_groupIndices.clear();
    m_skinToneVariants.clear();
    m_emojiIndices.clear();
    m_errorString.clear();

    QFile file(":/data/emoji.json");
    if (!file.open(QIODevice::ReadOnly)) {
        m_errorString = QStringLiteral("Could not open the embedded emoji catalogue.");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        m_errorString = QStringLiteral("Invalid emoji catalogue: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    m_unicodeVersion = root.value("unicodeVersion").toString();
    const QJsonArray jsonEntries = root.value("entries").toArray();
    const QHash<QString, QString> localizedKeywords = buildKeywordMap();
    const QHash<QString, QString> externalKeywords = loadExternalKeywords();
    QHash<QString, QString> groupPool;
    QHash<QString, QString> subgroupPool;
    const auto intern = [](QHash<QString, QString> &pool, const QString &value) {
        const auto existing = pool.constFind(value);
        if (existing != pool.cend())
            return existing.value();
        pool.insert(value, value);
        return value;
    };
    m_entries.reserve(jsonEntries.size());

    for (const QJsonValue &value : jsonEntries) {
        const QJsonObject object = value.toObject();
        EmojiEntry entry;
        entry.emoji = object.value("e").toString();
        entry.name = object.value("n").toString();
        entry.group = intern(groupPool, object.value("g").toString());
        entry.subgroup = intern(subgroupPool, object.value("s").toString());
        if (entry.emoji.isEmpty() || entry.group.isEmpty())
            continue;

        if (!m_groups.contains(entry.group))
            m_groups.append(entry.group);

        // Flags are painted from bundled Twemoji pixmaps (see EmojiCanvas), never
        // as text, so Segoe's glyph coverage is irrelevant to whether they show.
        entry.rendersLocally = entry.group == QLatin1String("Flags") || glyphRenders(entry.emoji);

        bool hasSkinTone = false;
        skinToneFamilyKey(entry.emoji, &hasSkinTone);
        if (!hasSkinTone && entry.rendersLocally) {
            const QString ownSearch = entry.emoji + QLatin1Char(' ') + entry.name + QLatin1Char(' ')
                + localizedKeywords.value(entry.emoji) + QLatin1Char(' ')
                + externalKeywords.value(keywordKey(entry.emoji));
            entry.ownSearchable = normalizeSearchText(ownSearch);
            const QString rawSearch = ownSearch + QLatin1Char(' ')
                + entry.group + QLatin1Char(' ') + entry.subgroup + QLatin1Char(' ')
                + groupSearchAliases(entry.group);
            entry.searchable = normalizeSearchText(rawSearch);
        }

        const int index = m_entries.size();
        m_emojiIndices.insert(entry.emoji, index);
        m_entries.append(std::move(entry));
    }

    // Unicode lists each Fitzpatrick combination as a separate fully-qualified
    // emoji. Keep those entries available for the Alt palette, but expose only
    // the canonical base in normal category/search grids.
    QHash<QString, int> familyBases;
    for (int index = 0; index < m_entries.size(); ++index) {
        bool hasTone = false;
        const QString familyKey = skinToneFamilyKey(m_entries.at(index).emoji, &hasTone);
        if (!hasTone)
            familyBases.insert(familyKey, index);
    }

    for (int index = 0; index < m_entries.size(); ++index) {
        EmojiEntry &entry = m_entries[index];
        bool hasTone = false;
        const QString familyKey = skinToneFamilyKey(entry.emoji, &hasTone);
        const int baseIndex = hasTone ? familyBases.value(familyKey, -1) : index;
        if (hasTone && baseIndex >= 0) {
            entry.isSkinToneVariant = true;
            entry.variantBase = m_entries.at(baseIndex).emoji;
            m_skinToneVariants[baseIndex].append(index);
        } else if (entry.rendersLocally) {
            m_groupIndices[entry.group].append(index);
        }
    }
    for (auto it = m_skinToneVariants.begin(); it != m_skinToneVariants.end(); ++it)
        it.value().prepend(it.key());

    if (m_entries.isEmpty()) {
        m_errorString = QStringLiteral("The embedded emoji catalogue is empty.");
        return false;
    }
    return true;
}

const QVector<int> &EmojiRepository::indicesForGroup(const QString &group) const {
    const auto it = m_groupIndices.constFind(group);
    return it == m_groupIndices.cend() ? EMPTY_INDICES : it.value();
}

const QVector<int> &EmojiRepository::skinToneVariantsFor(int entryIndex) const {
    const int baseIndex = baseIndexFor(entryIndex);
    const auto it = m_skinToneVariants.constFind(baseIndex);
    return it == m_skinToneVariants.cend() ? EMPTY_INDICES : it.value();
}

int EmojiRepository::baseIndexFor(int entryIndex) const {
    if (entryIndex < 0 || entryIndex >= m_entries.size())
        return -1;
    const EmojiEntry &entry = m_entries.at(entryIndex);
    return entry.isSkinToneVariant ? m_emojiIndices.value(entry.variantBase, -1) : entryIndex;
}

int EmojiRepository::indexOf(const QString &emoji) const {
    return m_emojiIndices.value(emoji, -1);
}

QString EmojiRepository::displayNameForGroup(const QString &group) {
    static const QHash<QString, QString> displayNames = {
        {"Smileys & Emotion", "Emoji & Emotion"},
        {"People & Body", "People & Body"},
        {"Animals & Nature", "Animals & Nature"},
        {"Food & Drink", "Food & Drink"},
        {"Travel & Places", "Travel & Places"},
        {"Activities", "Activities"},
        {"Objects", "Objects"},
        {"Symbols", "Symbols"},
        {"Flags", "Flags"},
    };
    return displayNames.value(group, group);
}

QString EmojiRepository::normalizeSearchText(const QString &text) {
    static const QRegularExpression separators(QStringLiteral("[_\\-/]+"));
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    QString normalized = text.normalized(QString::NormalizationForm_KC).toCaseFolded();
    normalized.replace(QChar(0x200c), QLatin1Char(' ')); // Persian ZWNJ
    normalized.replace(QChar(0x064a), QChar(0x06cc));   // Arabic Yeh -> Persian Yeh
    normalized.replace(QChar(0x0643), QChar(0x06a9));   // Arabic Kaf -> Persian Kaf
    normalized.replace(separators, QLatin1String(" "));
    normalized.replace(whitespace, QLatin1String(" "));
    return normalized.trimmed();
}
