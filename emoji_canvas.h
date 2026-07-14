#pragma once

#include "emoji_repository.h"

#include <QPoint>
#include <QRect>
#include <QTimer>
#include <QVector>
#include <QWidget>

struct EmojiSection {
    QString id;
    QString title;
    QVector<int> entryIndices;
};

class EmojiCanvas : public QWidget {
    Q_OBJECT

public:
    explicit EmojiCanvas(const EmojiRepository *repository, QWidget *parent = nullptr);

    void setSections(QVector<EmojiSection> sections);
    int sectionTop(const QString &id) const;
    QString sectionAt(int y) const;
    void selectFirst();
    void setScale(double scale);
    QSize sizeHint() const override;

signals:
    void emojiActivated(const QString &emoji, bool copyOnly);
    void variantsRequested(int repositoryIndex, const QRect &globalCellRect);
    // Right-click on a "Frequently Used" cell: offer removal instead of the
    // silent copy that right-click means elsewhere in the grid.
    void recentContextRequested(int repositoryIndex, const QRect &globalCellRect);
    // The user clicked while holding Alt (the skin-tone gesture). The picker has
    // no focus, so that Alt is landing in the app underneath and its release
    // would open that app's menu; the controller masks it.
    void altGestureUsed();
    void ensureVisibleRequested(const QRect &rect);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct LayoutSection {
        EmojiSection section;
        int top = 0;
        int headerTop = 0;
        int gridTop = 0;
        int rows = 0;
        int bottom = 0;
    };

    struct Hit {
        int section = -1;
        int item = -1;
        bool isValid() const { return section >= 0 && item >= 0; }
        bool operator==(const Hit &other) const {
            return section == other.section && item == other.item;
        }
        bool operator!=(const Hit &other) const { return !(*this == other); }
    };

    void rebuildLayout();
    void applyScaleMetrics();
    Hit hitTest(const QPoint &position) const;
    QRect cellRect(const Hit &hit) const;
    const EmojiEntry *entryFor(const Hit &hit) const;
    int repositoryIndexFor(const Hit &hit) const;
    Hit adjacentHit(int delta) const;
    void setCurrent(const Hit &hit, bool ensureVisible);

    const EmojiRepository *m_repository = nullptr;
    QVector<LayoutSection> m_layoutSections;
    Hit m_hovered;
    Hit m_current;
    QPoint m_lastGlobalMouse;
    QTimer m_tooltipTimer;
    int m_columns = 1;
    int m_xStart = 0;
    int m_contentHeight = 1;

    // Base metrics at scale 1.0; the m_* values below are these times m_scale so
    // the whole grid (cells, fonts, flags) zooms together with the panel.
    static constexpr int OUTER_MARGIN = 10;
    static constexpr int CELL_WIDTH = 44;
    static constexpr int CELL_HEIGHT = 43;
    static constexpr int HEADER_HEIGHT = 34;
    static constexpr int SECTION_GAP = 8;
    static constexpr int EMOJI_POINT = 22;
    static constexpr int HEADER_POINT = 10;

    double m_scale = 1.0;
    int m_cellWidth = CELL_WIDTH;
    int m_cellHeight = CELL_HEIGHT;
    int m_headerHeight = HEADER_HEIGHT;
    int m_sectionGap = SECTION_GAP;
    int m_outerMargin = OUTER_MARGIN;
    int m_emojiPointSize = EMOJI_POINT;
    int m_headerPointSize = HEADER_POINT;
};
