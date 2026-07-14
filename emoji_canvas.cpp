#include "emoji_canvas.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QToolTip>

#include <algorithm>

namespace {
QString flagAssetPath(const QString &emoji) {
    QStringList parts;
    const QList<uint> points = emoji.toUcs4();
    parts.reserve(points.size());
    for (uint point : points)
        parts.append(QString::number(point, 16));
    return QStringLiteral(":/flags/") + parts.join(QLatin1Char('-')) + QStringLiteral(".png");
}
} // namespace

EmojiCanvas::EmojiCanvas(const EmojiRepository *repository, QWidget *parent)
    : QWidget(parent), m_repository(repository) {
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_tooltipTimer.setSingleShot(true);
    m_tooltipTimer.setInterval(550);
    connect(&m_tooltipTimer, &QTimer::timeout, this, [this]() {
        const EmojiEntry *entry = entryFor(m_hovered);
        if (entry) {
            QString tooltip = entry->name;
            if (m_repository->skinToneVariantsFor(repositoryIndexFor(m_hovered)).size() > 1)
                tooltip += QStringLiteral("\nAlt+click for skin tones");
            QToolTip::showText(m_lastGlobalMouse, tooltip, this, cellRect(m_hovered), 2500);
        }
    });
}

void EmojiCanvas::setSections(QVector<EmojiSection> sections) {
    m_layoutSections.clear();
    m_layoutSections.reserve(sections.size());
    for (EmojiSection &section : sections) {
        if (section.entryIndices.isEmpty())
            continue;
        LayoutSection layout;
        layout.section = std::move(section);
        m_layoutSections.append(std::move(layout));
    }
    m_hovered = {};
    m_current = {};
    rebuildLayout();
    update();
}

QSize EmojiCanvas::sizeHint() const {
    return QSize(qRound(440 * m_scale), m_contentHeight);
}

void EmojiCanvas::setScale(double scale) {
    if (qFuzzyCompare(m_scale, scale))
        return;
    m_scale = scale;
    applyScaleMetrics();
    rebuildLayout();
    update();
}

void EmojiCanvas::applyScaleMetrics() {
    m_cellWidth = qRound(CELL_WIDTH * m_scale);
    m_cellHeight = qRound(CELL_HEIGHT * m_scale);
    m_headerHeight = qRound(HEADER_HEIGHT * m_scale);
    m_sectionGap = qRound(SECTION_GAP * m_scale);
    m_outerMargin = qRound(OUTER_MARGIN * m_scale);
    m_emojiPointSize = qRound(EMOJI_POINT * m_scale);
    m_headerPointSize = qRound(HEADER_POINT * m_scale);
}

void EmojiCanvas::rebuildLayout() {
    const int availableWidth = std::max(m_cellWidth, width() - 2 * m_outerMargin);
    m_columns = std::max(1, availableWidth / m_cellWidth);
    m_xStart = std::max(m_outerMargin, (width() - m_columns * m_cellWidth) / 2);

    int y = 4;
    for (LayoutSection &layout : m_layoutSections) {
        layout.top = y;
        layout.headerTop = y;
        layout.gridTop = y + m_headerHeight;
        layout.rows = (layout.section.entryIndices.size() + m_columns - 1) / m_columns;
        layout.bottom = layout.gridTop + layout.rows * m_cellHeight;
        y = layout.bottom + m_sectionGap;
    }
    m_contentHeight = std::max(1, y + 4);
    setMinimumHeight(m_contentHeight);
    setMaximumHeight(m_contentHeight);
    updateGeometry();
}

void EmojiCanvas::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    rebuildLayout();
}

void EmojiCanvas::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRect exposed = event->rect();

    QFont headerFont(QStringLiteral("Segoe UI"), m_headerPointSize);
    headerFont.setWeight(QFont::DemiBold);
    QFont emojiFont(QStringLiteral("Segoe UI Emoji"), m_emojiPointSize);

    for (int sectionIndex = 0; sectionIndex < m_layoutSections.size(); ++sectionIndex) {
        const LayoutSection &layout = m_layoutSections.at(sectionIndex);
        if (layout.bottom < exposed.top() || layout.top > exposed.bottom())
            continue;

        painter.setFont(headerFont);
        painter.setPen(QColor("#88a2b8"));
        const QRect headerRect(15, layout.headerTop, width() - 30, m_headerHeight);
        painter.drawText(headerRect, Qt::AlignVCenter | Qt::AlignLeft, layout.section.title);

        if (layout.gridTop > exposed.bottom() || layout.bottom < exposed.top())
            continue;

        const int firstRow = std::clamp((exposed.top() - layout.gridTop) / m_cellHeight, 0, layout.rows - 1);
        const int lastRow = std::clamp((exposed.bottom() - layout.gridTop) / m_cellHeight, 0, layout.rows - 1);
        painter.setFont(emojiFont);

        for (int row = firstRow; row <= lastRow; ++row) {
            for (int column = 0; column < m_columns; ++column) {
                const int itemIndex = row * m_columns + column;
                if (itemIndex >= layout.section.entryIndices.size())
                    break;

                const Hit hit{sectionIndex, itemIndex};
                const QRect rect = cellRect(hit);
                if (hit == m_hovered || hit == m_current) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(hit == m_current ? QColor("#2d5273") : QColor("#243443"));
                    painter.drawRoundedRect(rect.adjusted(2, 2, -2, -2), 8, 8);
                }

                const int repositoryIndex = layout.section.entryIndices.at(itemIndex);
                if (repositoryIndex < 0 || repositoryIndex >= m_repository->entries().size())
                    continue;
                const EmojiEntry &entry = m_repository->entries().at(repositoryIndex);
                if (entry.group == QLatin1String("Flags")) {
                    const QPixmap flag(flagAssetPath(entry.emoji));
                    if (!flag.isNull()) {
                        const QSize targetSize = flag.size().scaled(
                            QSize(qRound(32 * m_scale), qRound(27 * m_scale)), Qt::KeepAspectRatio);
                        const QRect target(QPoint(rect.center().x() - targetSize.width() / 2,
                                                  rect.center().y() - targetSize.height() / 2),
                                           targetSize);
                        painter.drawPixmap(target, flag, flag.rect());
                        continue;
                    }
                }
                painter.setPen(QColor("#ffffff"));
                painter.drawText(rect, Qt::AlignCenter, entry.emoji);
                if (m_repository->skinToneVariantsFor(repositoryIndex).size() > 1) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor("#55a8e8"));
                    const qreal markerOffset = 5.0 * m_scale;
                    const qreal markerRadius = std::max(2.0, 2.0 * m_scale);
                    painter.drawEllipse(QPointF(rect.right() - markerOffset, rect.bottom() - markerOffset),
                                        markerRadius, markerRadius);
                }
            }
        }
    }
}

EmojiCanvas::Hit EmojiCanvas::hitTest(const QPoint &position) const {
    if (position.x() < m_xStart || position.x() >= m_xStart + m_columns * m_cellWidth)
        return {};

    for (int sectionIndex = 0; sectionIndex < m_layoutSections.size(); ++sectionIndex) {
        const LayoutSection &layout = m_layoutSections.at(sectionIndex);
        if (position.y() < layout.gridTop || position.y() >= layout.bottom)
            continue;

        const int row = (position.y() - layout.gridTop) / m_cellHeight;
        const int column = (position.x() - m_xStart) / m_cellWidth;
        const int itemIndex = row * m_columns + column;
        if (itemIndex >= 0 && itemIndex < layout.section.entryIndices.size())
            return {sectionIndex, itemIndex};
        return {};
    }
    return {};
}

QRect EmojiCanvas::cellRect(const Hit &hit) const {
    if (!hit.isValid() || hit.section >= m_layoutSections.size())
        return {};
    const LayoutSection &layout = m_layoutSections.at(hit.section);
    const int row = hit.item / m_columns;
    const int column = hit.item % m_columns;
    return QRect(m_xStart + column * m_cellWidth, layout.gridTop + row * m_cellHeight,
                 m_cellWidth, m_cellHeight);
}

const EmojiEntry *EmojiCanvas::entryFor(const Hit &hit) const {
    if (!hit.isValid() || hit.section >= m_layoutSections.size())
        return nullptr;
    const QVector<int> &indices = m_layoutSections.at(hit.section).section.entryIndices;
    if (hit.item >= indices.size())
        return nullptr;
    const int repositoryIndex = indices.at(hit.item);
    if (repositoryIndex < 0 || repositoryIndex >= m_repository->entries().size())
        return nullptr;
    return &m_repository->entries().at(repositoryIndex);
}

int EmojiCanvas::repositoryIndexFor(const Hit &hit) const {
    if (!hit.isValid() || hit.section >= m_layoutSections.size())
        return -1;
    const QVector<int> &indices = m_layoutSections.at(hit.section).section.entryIndices;
    return hit.item >= 0 && hit.item < indices.size() ? indices.at(hit.item) : -1;
}

void EmojiCanvas::mouseMoveEvent(QMouseEvent *event) {
    const Hit hit = hitTest(event->position().toPoint());
    m_lastGlobalMouse = event->globalPosition().toPoint();
    if (hit != m_hovered) {
        const QRect oldRect = cellRect(m_hovered);
        m_hovered = hit;
        update(oldRect.united(cellRect(m_hovered)).adjusted(-2, -2, 2, 2));
        QToolTip::hideText();
        m_tooltipTimer.stop();
        if (m_hovered.isValid())
            m_tooltipTimer.start();
    }
    QWidget::mouseMoveEvent(event);
}

void EmojiCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    // Fires before the hit test: holding Alt reaches the app underneath whether
    // or not the click landed on an emoji, or on one that even has skin tones.
    if (event->modifiers().testFlag(Qt::AltModifier))
        emit altGestureUsed();

    const Hit hit = hitTest(event->position().toPoint());
    const EmojiEntry *entry = entryFor(hit);
    if (!entry)
        return;

    setCurrent(hit, false);
    const int repositoryIndex = repositoryIndexFor(hit);
    if (event->button() == Qt::RightButton
        && m_layoutSections.at(hit.section).section.id == QLatin1String("recent")) {
        const QRect localRect = cellRect(hit);
        emit recentContextRequested(repositoryIndex,
                                    QRect(mapToGlobal(localRect.topLeft()), localRect.size()));
        return;
    }
    if (event->button() == Qt::LeftButton
        && event->modifiers().testFlag(Qt::AltModifier)
        && m_repository->skinToneVariantsFor(repositoryIndex).size() > 1) {
        const QRect localRect = cellRect(hit);
        emit variantsRequested(repositoryIndex,
                               QRect(mapToGlobal(localRect.topLeft()), localRect.size()));
        return;
    }
    const bool copyOnly = event->button() == Qt::RightButton
        || event->modifiers().testFlag(Qt::ShiftModifier);
    emit emojiActivated(entry->emoji, copyOnly);
}

void EmojiCanvas::leaveEvent(QEvent *event) {
    const QRect oldRect = cellRect(m_hovered);
    m_hovered = {};
    m_tooltipTimer.stop();
    QToolTip::hideText();
    update(oldRect);
    QWidget::leaveEvent(event);
}

EmojiCanvas::Hit EmojiCanvas::adjacentHit(int delta) const {
    if (m_layoutSections.isEmpty())
        return {};
    if (!m_current.isValid())
        return {0, 0};

    int section = m_current.section;
    int item = m_current.item + delta;
    while (section >= 0 && section < m_layoutSections.size()) {
        const int count = m_layoutSections.at(section).section.entryIndices.size();
        if (item >= 0 && item < count)
            return {section, item};
        if (item < 0) {
            --section;
            if (section >= 0)
                item += m_layoutSections.at(section).section.entryIndices.size();
        } else {
            item -= count;
            ++section;
        }
    }
    return m_current;
}

void EmojiCanvas::setCurrent(const Hit &hit, bool ensureVisible) {
    if (!hit.isValid())
        return;
    const QRect oldRect = cellRect(m_current);
    m_current = hit;
    const QRect newRect = cellRect(m_current);
    update(oldRect.united(newRect).adjusted(-2, -2, 2, 2));
    if (ensureVisible)
        emit ensureVisibleRequested(newRect);
}

void EmojiCanvas::selectFirst() {
    if (m_layoutSections.isEmpty() || m_layoutSections.first().section.entryIndices.isEmpty())
        return;
    setFocus(Qt::ShortcutFocusReason);
    setCurrent({0, 0}, true);
}

void EmojiCanvas::keyPressEvent(QKeyEvent *event) {
    int delta = 0;
    switch (event->key()) {
    case Qt::Key_Left: delta = -1; break;
    case Qt::Key_Right: delta = 1; break;
    case Qt::Key_Up: delta = -m_columns; break;
    case Qt::Key_Down: delta = m_columns; break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (event->modifiers().testFlag(Qt::AltModifier)
            && m_repository->skinToneVariantsFor(repositoryIndexFor(m_current)).size() > 1) {
            const QRect localRect = cellRect(m_current);
            emit variantsRequested(repositoryIndexFor(m_current),
                                   QRect(mapToGlobal(localRect.topLeft()), localRect.size()));
        } else if (const EmojiEntry *entry = entryFor(m_current)) {
            emit emojiActivated(entry->emoji, event->modifiers().testFlag(Qt::ShiftModifier));
        }
        event->accept();
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    setCurrent(adjacentHit(delta), true);
    event->accept();
}

bool EmojiCanvas::event(QEvent *event) {
    if (event->type() == QEvent::ToolTip) {
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

int EmojiCanvas::sectionTop(const QString &id) const {
    for (const LayoutSection &layout : m_layoutSections) {
        if (layout.section.id == id)
            return layout.top;
    }
    return -1;
}

QString EmojiCanvas::sectionAt(int y) const {
    QString result;
    for (const LayoutSection &layout : m_layoutSections) {
        if (layout.top <= y)
            result = layout.section.id;
        else
            break;
    }
    return result;
}
