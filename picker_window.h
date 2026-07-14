#pragma once

#include "emoji_canvas.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <QWidget>

class EmojiRepository;
class UsageStore;
class QAbstractButton;
class QCloseEvent;
class QEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QScrollArea;
class QStackedWidget;
class QTimer;

class PickerWindow : public QWidget {
    Q_OBJECT

public:
    PickerWindow(const EmojiRepository *repository, UsageStore *usage, QWidget *parent = nullptr);

    void prepareForShow();
    void scrollToSection(const QString &sectionId);
    void setShortcutHint(const QString &shortcut);
    void suspendAutoHideForInsertion();
    void resumeAfterInsertion();

    // Focus-on-demand: the window floats passively (no focus) until the user
    // clicks Search, at which point the controller activates it and calls this.
    void beginTypingMode();
    bool isTypingMode() const { return m_typingMode; }
    bool isVariantMenuOpen() const { return m_variantMenuOpen; }
    void dismiss();

    void setPanelSizeIndex(int index);
    int panelSizeIndex() const { return m_panelSizeIndex; }
    void cyclePanelSize(int delta);
    static QStringList panelSizeLabels();

signals:
    void emojiChosen(const QString &emoji, bool copyOnly);
    void hiddenByUser();
    void searchFocusRequested();
    void panelSizeChanged(int index);

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupUi();
    void applyStyles(double scale);
    void rebuildNormalSections();
    void rebuildSearchResults();
    QVector<int> rankedRecent(int maximum) const;
    QVector<int> search(const QString &query) const;
    void navigateTo(const QString &sectionId);
    void updateActiveCategory();
    void setActiveCategory(const QString &sectionId);
    void showVariantMenu(int repositoryIndex, const QRect &globalCellRect);
    void showRecentContextMenu(int repositoryIndex, const QRect &globalCellRect);
    void recordUsage(const QString &emoji);
    void hidePicker();
    void applyChromeScale(double scale);

    const EmojiRepository *m_repository = nullptr;
    UsageStore *m_usage = nullptr;
    QWidget *m_header = nullptr;
    QWidget *m_searchOuter = nullptr;
    QLabel *m_shortcutHint = nullptr;
    QLineEdit *m_search = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    EmojiCanvas *m_canvas = nullptr;
    QStackedWidget *m_contentStack = nullptr;
    QWidget *m_emptyState = nullptr;
    QTimer *m_searchTimer = nullptr;
    QHash<QString, QAbstractButton *> m_categoryButtons;
    QSet<QString> m_sessionRecorded;
    bool m_searchMode = false;
    bool m_autoHideArmed = false;
    bool m_autoHideSuppressed = false;
    bool m_variantMenuOpen = false;
    bool m_variantSelectionInProgress = false;
    bool m_typingMode = false;
    int m_panelSizeIndex = 0;
};
