#include "picker_window.h"

#include "emoji_repository.h"
#include "usage_store.h"
#include "windows_integration.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>

namespace {
const QColor MUTED_COLOR("#6f8497");
const QColor ACTIVE_COLOR("#55a8e8");

// Panel-size presets. Each scales the window, the emoji grid and the chrome
// together so the whole panel zooms as one. Scaling the chrome is what lets the
// range go below 1.0 — the search + category row has a minimum width that would
// otherwise not fit in a smaller window.
struct PanelPreset {
    const char *label;
    double scale;
};
const QVector<PanelPreset> PANEL_PRESETS = {
    {"Small", 0.72},
    {"Medium", 0.85},
    {"Large", 1.00},
    {"Extra Large", 1.20},
    {"Huge", 1.45},
};
constexpr int DEFAULT_PANEL_INDEX = 1; // Medium
constexpr int BASE_PANEL_WIDTH = 466;
constexpr int BASE_PANEL_HEIGHT = 626;
constexpr int BASE_HEADER_HEIGHT = 48;
constexpr int BASE_SEARCH_HEIGHT = 54;
constexpr int BASE_SEARCH_MIN_WIDTH = 118;
constexpr int BASE_CATEGORY_WIDTH = 30;
constexpr int BASE_CATEGORY_HEIGHT = 34;

enum class CategoryIcon {
    Recent,
    Smile,
    People,
    Nature,
    Food,
    Travel,
    Activity,
    Objects,
    Symbols,
    Flags,
};

class DragHeader final : public QWidget {
public:
    explicit DragHeader(QWidget *parent = nullptr) : QWidget(parent) {
        setCursor(Qt::OpenHandCursor);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            m_offset = event->globalPosition().toPoint() - window()->frameGeometry().topLeft();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (m_dragging && event->buttons().testFlag(Qt::LeftButton)) {
            window()->move(event->globalPosition().toPoint() - m_offset);
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragging = false;
            setCursor(Qt::OpenHandCursor);
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QPoint m_offset;
    bool m_dragging = false;
};

class SearchEdit final : public QLineEdit {
public:
    explicit SearchEdit(QWidget *parent = nullptr) : QLineEdit(parent) {
        setTextMargins(26, 0, 4, 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QLineEdit::paintEvent(event);
        // The panel-size scale is published as a property so the magnifier zooms
        // with everything else instead of staying a fixed size.
        const QVariant scaleProperty = property("uiScale");
        const double scale = scaleProperty.isValid() ? scaleProperty.toDouble() : 1.0;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(hasFocus() ? ACTIVE_COLOR : MUTED_COLOR, 1.6 * scale);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const double midY = height() / 2.0;
        painter.drawEllipse(QRectF(9.0 * scale, midY - 6.0 * scale,
                                   10.0 * scale, 10.0 * scale));
        painter.drawLine(QPointF(17.0 * scale, midY + 3.0 * scale),
                         QPointF(21.0 * scale, midY + 7.0 * scale));
    }
};

class CategoryButton final : public QAbstractButton {
public:
    CategoryButton(CategoryIcon icon, QWidget *parent = nullptr) : QAbstractButton(parent), m_icon(icon) {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(30, 34);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor color = isChecked() ? ACTIVE_COLOR : (underMouse() ? QColor("#9db1c2") : MUTED_COLOR);
        if (underMouse() || isChecked()) {
            p.setPen(Qt::NoPen);
            p.setBrush(isChecked() ? QColor(55, 139, 203, 45) : QColor(255, 255, 255, 12));
            p.drawRoundedRect(rect().adjusted(1, 2, -1, -2), 8, 8);
        }

        QPen pen(color, 1.55, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.translate(width() / 2.0, height() / 2.0);
        drawIcon(p);
    }

private:
    void drawIcon(QPainter &p) const {
        switch (m_icon) {
        case CategoryIcon::Recent:
            p.drawEllipse(QRectF(-7, -7, 14, 14));
            p.drawLine(QPointF(0, -4), QPointF(0, 0));
            p.drawLine(QPointF(0, 0), QPointF(4, 2));
            break;
        case CategoryIcon::Smile:
            p.drawEllipse(QRectF(-7, -7, 14, 14));
            p.drawPoint(QPointF(-2.5, -2));
            p.drawPoint(QPointF(2.5, -2));
            p.drawArc(QRectF(-4, -1, 8, 6), 200 * 16, 140 * 16);
            break;
        case CategoryIcon::People:
            p.drawEllipse(QRectF(-3, -7, 6, 6));
            p.drawArc(QRectF(-7, 0, 14, 10), 0, 180 * 16);
            break;
        case CategoryIcon::Nature:
            p.drawEllipse(QRectF(-7, -1, 6, 6));
            p.drawEllipse(QRectF(1, -1, 6, 6));
            p.drawEllipse(QRectF(-5, -7, 5, 5));
            p.drawEllipse(QRectF(0, -8, 5, 5));
            p.drawEllipse(QRectF(-4, 3, 8, 6));
            break;
        case CategoryIcon::Food:
            p.drawEllipse(QRectF(-6, -5, 12, 12));
            p.drawLine(QPointF(0, -5), QPointF(2, -9));
            p.drawArc(QRectF(1, -9, 6, 5), 20 * 16, 140 * 16);
            break;
        case CategoryIcon::Travel: {
            QPainterPath path;
            path.moveTo(-8, 1);
            path.lineTo(7, -7);
            path.lineTo(3, 1);
            path.lineTo(8, 6);
            path.lineTo(0, 3);
            path.lineTo(-4, 8);
            path.lineTo(-4, 2);
            path.closeSubpath();
            p.drawPath(path);
            break;
        }
        case CategoryIcon::Activity:
            p.drawEllipse(QRectF(-7, -7, 14, 14));
            p.drawArc(QRectF(-5, -8, 10, 16), 70 * 16, 80 * 16);
            p.drawArc(QRectF(-5, -8, 10, 16), 250 * 16, 80 * 16);
            p.drawLine(QPointF(-6, -3), QPointF(6, 3));
            break;
        case CategoryIcon::Objects:
            p.drawEllipse(QRectF(-5, -8, 10, 11));
            p.drawLine(QPointF(-3, 4), QPointF(3, 4));
            p.drawLine(QPointF(-2, 7), QPointF(2, 7));
            break;
        case CategoryIcon::Symbols:
            p.drawLine(QPointF(-5, -8), QPointF(-7, 8));
            p.drawLine(QPointF(3, -8), QPointF(1, 8));
            p.drawLine(QPointF(-8, -3), QPointF(7, -3));
            p.drawLine(QPointF(-8, 3), QPointF(7, 3));
            break;
        case CategoryIcon::Flags:
            p.drawLine(QPointF(-6, -8), QPointF(-6, 8));
            p.drawLine(QPointF(-6, -7), QPointF(6, -5));
            p.drawLine(QPointF(6, -5), QPointF(3, 1));
            p.drawLine(QPointF(3, 1), QPointF(-6, -1));
            break;
        }
    }

    CategoryIcon m_icon;
};

struct CategoryDescriptor {
    QString id;
    QString tooltip;
    CategoryIcon icon;
};

const QVector<CategoryDescriptor> CATEGORIES = {
    {"recent", "Frequently Used", CategoryIcon::Recent},
    {"Smileys & Emotion", "Emoji & Emotion", CategoryIcon::Smile},
    {"People & Body", "People & Body", CategoryIcon::People},
    {"Animals & Nature", "Animals & Nature", CategoryIcon::Nature},
    {"Food & Drink", "Food & Drink", CategoryIcon::Food},
    {"Travel & Places", "Travel & Places", CategoryIcon::Travel},
    {"Activities", "Activities", CategoryIcon::Activity},
    {"Objects", "Objects", CategoryIcon::Objects},
    {"Symbols", "Symbols", CategoryIcon::Symbols},
    {"Flags", "Flags", CategoryIcon::Flags},
};
} // namespace

PickerWindow::PickerWindow(const EmojiRepository *repository, UsageStore *usage, QWidget *parent)
    : QWidget(parent), m_repository(repository), m_usage(usage) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    // Show without stealing focus from the app the user is typing in; the
    // controller also applies WS_EX_NOACTIVATE so clicks never activate us.
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFixedSize(BASE_PANEL_WIDTH, BASE_PANEL_HEIGHT);
    setupUi();
    applyStyles(1.0);
    rebuildNormalSections();

    const int savedSizeIndex =
        QSettings(QStringLiteral("PickMoji"), QStringLiteral("PickMoji"))
            .value(QStringLiteral("panelSizeLevel2"), DEFAULT_PANEL_INDEX).toInt();
    setPanelSizeIndex(savedSizeIndex);

    connect(m_usage, &UsageStore::usageChanged, this, [this]() {
        if (!isVisible() && !m_searchMode) {
            rebuildNormalSections();
        }
    });
}

void PickerWindow::setupUi() {
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *panel = new QFrame(this);
    panel->setObjectName("panel");
    rootLayout->addWidget(panel);

    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    auto *header = new DragHeader(panel);
    header->setObjectName("header");
    header->setFixedHeight(BASE_HEADER_HEIGHT);
    m_header = header;
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(20, 0, 16, 0);
    headerLayout->setSpacing(8);

    auto *title = new QLabel("Emoji", header);
    title->setObjectName("headerTitle");
    title->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerLayout->addWidget(title);
    headerLayout->addStretch();

    m_shortcutHint = new QLabel(header);
    m_shortcutHint->setObjectName("shortcutHint");
    m_shortcutHint->setAlignment(Qt::AlignCenter);
    m_shortcutHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerLayout->addWidget(m_shortcutHint);
    panelLayout->addWidget(header);

    auto *searchOuter = new QWidget(panel);
    searchOuter->setObjectName("searchOuter");
    searchOuter->setFixedHeight(BASE_SEARCH_HEIGHT);
    m_searchOuter = searchOuter;
    auto *searchOuterLayout = new QHBoxLayout(searchOuter);
    searchOuterLayout->setContentsMargins(10, 7, 10, 7);
    searchOuterLayout->setSpacing(6);

    auto *searchBar = new QWidget(searchOuter);
    searchBar->setObjectName("searchBar");
    auto *searchLayout = new QHBoxLayout(searchBar);
    searchLayout->setContentsMargins(0, 0, 5, 0);
    searchLayout->setSpacing(0);

    m_search = new SearchEdit(searchBar);
    m_search->setObjectName("searchInput");
    m_search->setPlaceholderText("Search");
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(BASE_SEARCH_MIN_WIDTH);
    m_search->installEventFilter(this);
    searchLayout->addWidget(m_search, 1);

    for (const CategoryDescriptor &descriptor : CATEGORIES) {
        auto *button = new CategoryButton(descriptor.icon, searchBar);
        button->setToolTip(descriptor.tooltip);
        button->setAccessibleName(descriptor.tooltip);
        searchLayout->addWidget(button);
        m_categoryButtons.insert(descriptor.id, button);
        connect(button, &QAbstractButton::clicked, this, [this, id = descriptor.id]() {
            navigateTo(id);
        });
    }

    searchOuterLayout->addWidget(searchBar);
    panelLayout->addWidget(searchOuter);

    m_scrollArea = new QScrollArea(panel);
    m_scrollArea->setObjectName("emojiScroll");
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_canvas = new EmojiCanvas(m_repository, m_scrollArea);
    m_scrollArea->setWidget(m_canvas);

    m_emptyState = new QWidget(panel);
    auto *emptyLayout = new QVBoxLayout(m_emptyState);
    emptyLayout->setContentsMargins(30, 30, 30, 30);
    emptyLayout->addStretch();
    auto *emptyIcon = new QLabel(QStringLiteral("⌕"), m_emptyState);
    emptyIcon->setObjectName("emptyIcon");
    emptyIcon->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyIcon);
    auto *emptyTitle = new QLabel("No emoji found", m_emptyState);
    emptyTitle->setObjectName("emptyTitle");
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyTitle);
    auto *emptyHint = new QLabel("Try another English or Persian keyword", m_emptyState);
    emptyHint->setObjectName("emptyHint");
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyHint);
    emptyLayout->addStretch();

    m_contentStack = new QStackedWidget(panel);
    m_contentStack->addWidget(m_scrollArea);
    m_contentStack->addWidget(m_emptyState);
    panelLayout->addWidget(m_contentStack, 1);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(55);
    connect(m_searchTimer, &QTimer::timeout, this, &PickerWindow::rebuildSearchResults);
    connect(m_search, &QLineEdit::textChanged, this, [this]() { m_searchTimer->start(); });

    connect(m_canvas, &EmojiCanvas::emojiActivated, this, [this](const QString &emoji, bool copyOnly) {
        recordUsage(emoji);
        emit emojiChosen(emoji, copyOnly);
    });
    connect(m_canvas, &EmojiCanvas::variantsRequested,
            this, &PickerWindow::showVariantMenu);
    connect(m_canvas, &EmojiCanvas::recentContextRequested,
            this, &PickerWindow::showRecentContextMenu);
    connect(m_canvas, &EmojiCanvas::altGestureUsed, this, &PickerWindow::altGestureUsed);
    connect(m_canvas, &EmojiCanvas::ensureVisibleRequested, this, [this](const QRect &rect) {
        QScrollBar *bar = m_scrollArea->verticalScrollBar();
        const int viewportTop = bar->value();
        const int viewportBottom = viewportTop + m_scrollArea->viewport()->height();
        if (rect.top() < viewportTop)
            bar->setValue(std::max(0, rect.top() - 8));
        else if (rect.bottom() > viewportBottom)
            bar->setValue(rect.bottom() - m_scrollArea->viewport()->height() + 8);
    });
    connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &PickerWindow::updateActiveCategory);
}

void PickerWindow::showVariantMenu(int repositoryIndex, const QRect &globalCellRect) {
    const QVector<int> &variants = m_repository->skinToneVariantsFor(repositoryIndex);
    if (variants.size() <= 1)
        return;

    m_variantMenuOpen = true;
    m_variantSelectionInProgress = false;
    m_autoHideArmed = false;

    auto *menu = new QMenu(this);
    menu->setObjectName("variantMenu");
    menu->setStyleSheet(R"(
        QMenu#variantMenu {
            background: #1d2a36;
            border: 1px solid #3b5062;
            border-radius: 10px;
            padding: 5px;
        }
        QMenu#variantMenu QPushButton {
            background: transparent;
            border: none;
            border-radius: 7px;
            font: 23px "Segoe UI Emoji";
        }
        QMenu#variantMenu QPushButton:hover { background: #2b4052; }
        QMenu#variantMenu QPushButton:pressed { background: #315c7d; }
    )");

    auto *content = new QWidget(menu);
    auto *grid = new QGridLayout(content);
    grid->setContentsMargins(3, 3, 3, 3);
    grid->setHorizontalSpacing(2);
    grid->setVerticalSpacing(2);
    const int columns = variants.size() <= 6 ? static_cast<int>(variants.size()) : 5;

    for (int item = 0; item < variants.size(); ++item) {
        const int variantIndex = variants.at(item);
        const EmojiEntry &entry = m_repository->entries().at(variantIndex);
        auto *button = new QPushButton(entry.emoji, content);
        button->setFixedSize(42, 42);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(entry.name);
        grid->addWidget(button, item / columns, item % columns);
        connect(button, &QPushButton::clicked, this, [this, menu, variantIndex]() {
            const QString emoji = m_repository->entries().at(variantIndex).emoji;
            m_variantSelectionInProgress = true;
            recordUsage(emoji);
            emit emojiChosen(emoji, false);
            menu->close();
        });
    }

    auto *widgetAction = new QWidgetAction(menu);
    widgetAction->setDefaultWidget(content);
    menu->addAction(widgetAction);
    connect(menu, &QMenu::aboutToHide, this, [this, menu]() {
        m_variantMenuOpen = false;
        m_variantSelectionInProgress = false;
        menu->deleteLater();
        // Dismissal is handled by the controller's click-away watchdog; closing
        // the tone palette should not, on its own, hide the picker.
    });

    // In passive mode the target app owns the foreground and must keep it:
    // an activating popup would yank focus from the app, and inserting the
    // chosen tone would then need a foreground round-trip — a blink at best,
    // a lost paste at worst. Mouse input works fine on a non-activating
    // window. Typing mode keeps the default so the picker stays focused.
    if (!m_typingMode) {
        menu->setAttribute(Qt::WA_ShowWithoutActivating, true);
        WindowsIntegration::setWindowNoActivate(menu->winId(), true);
    }

    menu->ensurePolished();
    const QSize popupSize = menu->sizeHint();
    QPoint popupPosition = globalCellRect.bottomLeft() + QPoint(0, 4);
    if (QScreen *screen = QGuiApplication::screenAt(popupPosition)) {
        const QRect available = screen->availableGeometry();
        if (popupPosition.x() + popupSize.width() > available.right())
            popupPosition.setX(available.right() - popupSize.width());
        if (popupPosition.y() + popupSize.height() > available.bottom())
            popupPosition.setY(globalCellRect.top() - popupSize.height() - 4);
        popupPosition.setX(std::max(available.left(), popupPosition.x()));
        popupPosition.setY(std::max(available.top(), popupPosition.y()));
    }
    menu->popup(popupPosition);
}

void PickerWindow::recordUsage(const QString &emoji) {
    // One count per panel-open: sending the same emoji five times in a row is
    // one act of choosing it, and counting every repeat would let a single
    // spree bulldoze the frequently-used ranking.
    if (m_sessionRecorded.contains(emoji))
        return;
    m_sessionRecorded.insert(emoji);
    m_usage->record(emoji);
}

void PickerWindow::showRecentContextMenu(int repositoryIndex, const QRect &globalCellRect) {
    if (repositoryIndex < 0 || repositoryIndex >= m_repository->entries().size())
        return;
    const QString emoji = m_repository->entries().at(repositoryIndex).emoji;

    // Reuse the variant-menu guard: while a popup of ours is up, the
    // controller's click-away watchdog must stand down.
    m_variantMenuOpen = true;
    m_autoHideArmed = false;

    auto *menu = new QMenu(this);
    menu->setObjectName("recentMenu");
    menu->setStyleSheet(R"(
        QMenu#recentMenu {
            background: #1d2a36;
            border: 1px solid #3b5062;
            border-radius: 8px;
            padding: 5px;
        }
        QMenu#recentMenu::item {
            color: #d3dee8;
            background: transparent;
            padding: 7px 16px;
            border-radius: 6px;
            font: 12px "Segoe UI";
        }
        QMenu#recentMenu::item:selected { background: #2b4052; }
    )");

    QAction *copyAction = menu->addAction(QStringLiteral("Copy %1").arg(emoji));
    connect(copyAction, &QAction::triggered, this, [this, emoji]() {
        emit emojiChosen(emoji, true);
    });

    QAction *removeAction = menu->addAction(QStringLiteral("Remove from Frequently Used"));
    connect(removeAction, &QAction::triggered, this, [this, repositoryIndex]() {
        // Recents aggregate a whole skin-tone family; removing only the shown
        // variant would let another tone pop straight back into its place.
        const QVector<int> &family = m_repository->skinToneVariantsFor(repositoryIndex);
        if (family.isEmpty()) {
            m_usage->remove(m_repository->entries().at(repositoryIndex).emoji);
        } else {
            for (int variantIndex : family)
                m_usage->remove(m_repository->entries().at(variantIndex).emoji);
        }
        if (!m_searchMode)
            rebuildNormalSections();
    });

    connect(menu, &QMenu::aboutToHide, this, [this, menu]() {
        m_variantMenuOpen = false;
        menu->deleteLater();
    });
    // Same as the tone palette: never activate over a passive picker, so the
    // target app keeps the foreground throughout.
    if (!m_typingMode) {
        menu->setAttribute(Qt::WA_ShowWithoutActivating, true);
        WindowsIntegration::setWindowNoActivate(menu->winId(), true);
    }
    menu->popup(globalCellRect.bottomLeft() + QPoint(2, 2));
}

void PickerWindow::applyStyles(double scale) {
    // Every radius/font/padding is derived from the scale. Leaving these fixed
    // while the widgets shrink is what made the search bar lose its rounding: a
    // fixed 18px radius exceeds half the height of a shorter bar, and Qt then
    // drops the rounding entirely and paints it square.
    const auto px = [scale](double base) {
        return QString::number(std::max(1, qRound(base * scale))) + QStringLiteral("px");
    };

    // Keep the pill just under half the bar's height so it always rounds.
    const int searchBarHeight = qRound((BASE_SEARCH_HEIGHT - 2 * 7) * scale);
    const QString searchRadius =
        QString::number(std::max(4, searchBarHeight / 2 - 1)) + QStringLiteral("px");

    setStyleSheet(
        QStringLiteral("#panel { background: #17212b; border: 1px solid #273747;"
                       " border-radius: ") + px(12) + QStringLiteral("; }")
        + QStringLiteral("#header { background: #17212b; border-bottom: 1px solid #253342;"
                         " border-top-left-radius: ") + px(12)
        + QStringLiteral("; border-top-right-radius: ") + px(12) + QStringLiteral("; }")
        + QStringLiteral("#headerTitle { color: #60b5f4; font: 600 ") + px(15)
        + QStringLiteral(" \"Segoe UI\"; }")
        + QStringLiteral("#shortcutHint { color: #8298aa; background: #202e3b;"
                         " border: 1px solid #2b3d4d; border-radius: ") + px(8)
        + QStringLiteral("; padding: ") + px(3) + QStringLiteral(" ") + px(8)
        + QStringLiteral("; font: ") + px(11) + QStringLiteral(" \"Segoe UI\"; }")
        + QStringLiteral("#searchOuter { background: #17212b; }")
        + QStringLiteral("#searchBar { background: #222f3e; border: 1px solid #28394a;"
                         " border-radius: ") + searchRadius + QStringLiteral("; }")
        + QStringLiteral("#searchBar:focus-within { border-color: #3d7198; }")
        + QStringLiteral("#searchInput { color: #d3dee8; background: transparent; border: none;"
                         " selection-background-color: #3c7eae; font: ") + px(13)
        + QStringLiteral(" \"Segoe UI\"; }")
        + QStringLiteral("#emojiScroll, #emojiScroll > QWidget > QWidget {"
                         " background: transparent; border: none; }")
        + QStringLiteral("QScrollBar:vertical { width: ") + px(6)
        + QStringLiteral("; margin: 3px 1px 3px 0; background: transparent; }")
        + QStringLiteral("QScrollBar::handle:vertical { min-height: ") + px(32)
        + QStringLiteral("; background: #34495b; border-radius: ") + px(3)
        + QStringLiteral("; }")
        + QStringLiteral("QScrollBar::handle:vertical:hover { background: #49657a; }")
        + QStringLiteral("QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical"
                         " { height: 0; }")
        + QStringLiteral("QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical"
                         " { background: transparent; }")
        + QStringLiteral("#emptyIcon { color: #536b7f; font: ") + px(42)
        + QStringLiteral(" \"Segoe UI\"; }")
        + QStringLiteral("#emptyTitle { color: #b2c1cd; font: 600 ") + px(14)
        + QStringLiteral(" \"Segoe UI\"; }")
        + QStringLiteral("#emptyHint { color: #657b8d; font: ") + px(12)
        + QStringLiteral(" \"Segoe UI\"; }")
        + QStringLiteral("QToolTip { color: #dbe7ef; background: #23313f;"
                         " border: 1px solid #3b4d5e; border-radius: 5px; padding: 5px;"
                         " font: 12px \"Segoe UI\"; }"));
}

QVector<int> PickerWindow::rankedRecent(int maximum) const {
    struct RankedFamily {
        int displayIndex = -1;
        double familyScore = 0.0;
        double displayScore = 0.0;
    };
    QHash<int, RankedFamily> families;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QVector<EmojiEntry> &entries = m_repository->entries();
    for (int i = 0; i < entries.size(); ++i) {
        const double value = m_usage->score(entries.at(i).emoji, now);
        if (value <= 0.01)
            continue;
        const int familyIndex = m_repository->baseIndexFor(i);
        RankedFamily &family = families[familyIndex];
        family.familyScore += value;
        if (family.displayIndex < 0 || value > family.displayScore) {
            family.displayIndex = i;
            family.displayScore = value;
        }
    }

    QVector<RankedFamily> ranked;
    ranked.reserve(families.size());
    for (auto it = families.cbegin(); it != families.cend(); ++it)
        ranked.append(it.value());
    std::sort(ranked.begin(), ranked.end(), [](const RankedFamily &a, const RankedFamily &b) {
        return a.familyScore > b.familyScore;
    });

    QVector<int> result;
    result.reserve(std::min(maximum, static_cast<int>(ranked.size())));
    for (int i = 0; i < ranked.size() && i < maximum; ++i)
        result.append(ranked.at(i).displayIndex);
    return result;
}

void PickerWindow::rebuildNormalSections() {
    m_searchMode = false;
    QVector<EmojiSection> sections;
    const QVector<int> recent = rankedRecent(48);
    if (!recent.isEmpty())
        sections.append({"recent", "Frequently Used", recent});

    for (const QString &group : m_repository->groups()) {
        sections.append({group, EmojiRepository::displayNameForGroup(group),
                         m_repository->indicesForGroup(group)});
    }
    m_canvas->setSections(std::move(sections));
    m_contentStack->setCurrentWidget(m_scrollArea);
    QTimer::singleShot(0, this, &PickerWindow::updateActiveCategory);
}

QVector<int> PickerWindow::search(const QString &rawQuery) const {
    const QString query = EmojiRepository::normalizeSearchText(rawQuery);
    const QStringList terms = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (terms.isEmpty())
        return {};

    struct Match { int index; double score; };
    QVector<Match> matches;
    const QVector<EmojiEntry> &entries = m_repository->entries();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (int i = 0; i < entries.size(); ++i) {
        const EmojiEntry &entry = entries.at(i);
        if (entry.isSkinToneVariant)
            continue;
        bool includesAll = true;
        for (const QString &term : terms) {
            if (!entry.searchable.contains(term)) {
                includesAll = false;
                break;
            }
        }
        if (!includesAll)
            continue;

        const QString name = EmojiRepository::normalizeSearchText(entry.name);
        const QString subgroup = EmojiRepository::normalizeSearchText(entry.subgroup);
        double rank = 10.0;
        if (entry.emoji == rawQuery.trimmed())
            rank += 1000.0;
        if (name == query)
            rank += 220.0;
        else if (name.startsWith(query))
            rank += 130.0;
        else if (name.contains(QLatin1Char(' ') + query))
            rank += 85.0;
        else if (subgroup.startsWith(query))
            rank += 55.0;
        // An emoji matched through its own name/keywords is what the user meant;
        // one swept in only by a category keyword is a bystander. Large enough
        // to also outweigh the subgroup and usage bonuses combined.
        bool ownMatch = true;
        for (const QString &term : terms) {
            if (!entry.ownSearchable.contains(term)) {
                ownMatch = false;
                break;
            }
        }
        if (ownMatch)
            rank += 90.0;
        rank += std::min(30.0, m_usage->score(entry.emoji, now) * 2.0);
        matches.append({i, rank});
    }

    std::stable_sort(matches.begin(), matches.end(), [&entries](const Match &a, const Match &b) {
        if (a.score != b.score)
            return a.score > b.score;
        return entries.at(a.index).name < entries.at(b.index).name;
    });

    QVector<int> result;
    result.reserve(matches.size());
    for (const Match &match : matches)
        result.append(match.index);
    return result;
}

void PickerWindow::rebuildSearchResults() {
    const QString query = m_search->text().trimmed();
    if (query.isEmpty()) {
        const int oldPosition = m_scrollArea->verticalScrollBar()->value();
        rebuildNormalSections();
        m_scrollArea->verticalScrollBar()->setValue(oldPosition);
        return;
    }

    m_searchMode = true;
    const QVector<int> results = search(query);
    if (results.isEmpty()) {
        m_canvas->setSections({});
        m_contentStack->setCurrentWidget(m_emptyState);
    } else {
        m_canvas->setSections({{"search", QString("Search Results · %1").arg(results.size()), results}});
        m_contentStack->setCurrentWidget(m_scrollArea);
        m_scrollArea->verticalScrollBar()->setValue(0);
    }
    setActiveCategory(QString());
}

void PickerWindow::navigateTo(const QString &sectionId) {
    if (!m_search->text().isEmpty()) {
        // No QSignalBlocker here: blocking the line edit's signals also freezes
        // its clear-button animation, leaving a stale ✕ over the empty field.
        // Let textChanged fire, then cancel the rebuild it scheduled — we
        // rebuild ourselves right away.
        m_search->clear();
        m_searchTimer->stop();
        rebuildNormalSections();
    }

    int top = m_canvas->sectionTop(sectionId);
    if (top < 0 && sectionId == "recent")
        top = 0;
    if (top >= 0)
        m_scrollArea->verticalScrollBar()->setValue(top);
    setActiveCategory(sectionId);
}

void PickerWindow::updateActiveCategory() {
    if (m_searchMode)
        return;
    const int probe = m_scrollArea->verticalScrollBar()->value() + 24;
    setActiveCategory(m_canvas->sectionAt(probe));
}

void PickerWindow::setActiveCategory(const QString &sectionId) {
    for (auto it = m_categoryButtons.begin(); it != m_categoryButtons.end(); ++it)
        it.value()->setChecked(it.key() == sectionId);
}

void PickerWindow::prepareForShow() {
    m_autoHideSuppressed = false;
    m_autoHideArmed = false;
    m_typingMode = false;
    m_sessionRecorded.clear(); // usage counting is per panel-open
    // Signals stay unblocked so the clear button hides itself (see navigateTo).
    m_search->clear();
    m_searchTimer->stop();
    rebuildNormalSections();
    m_scrollArea->verticalScrollBar()->setValue(0);
    // Passive show: the target app keeps keyboard focus so click-to-insert
    // never bounces the foreground. Focus is only taken on Search click.
}

void PickerWindow::beginTypingMode() {
    if (!isVisible())
        return;
    m_typingMode = true;
    activateWindow();
    m_search->setFocus(Qt::MouseFocusReason);
    m_autoHideArmed = true;
}

void PickerWindow::dismiss() {
    hidePicker();
}

void PickerWindow::applyChromeScale(double scale) {
    if (m_header) {
        m_header->setFixedHeight(qRound(BASE_HEADER_HEIGHT * scale));
        if (m_header->layout()) {
            m_header->layout()->setContentsMargins(qRound(20 * scale), 0,
                                                   qRound(16 * scale), 0);
        }
    }
    if (m_searchOuter) {
        m_searchOuter->setFixedHeight(qRound(BASE_SEARCH_HEIGHT * scale));
        // These paddings must scale too, otherwise the search bar gets squeezed
        // disproportionately at smaller sizes.
        if (m_searchOuter->layout()) {
            m_searchOuter->layout()->setContentsMargins(qRound(10 * scale), qRound(7 * scale),
                                                        qRound(10 * scale), qRound(7 * scale));
        }
    }
    if (m_search) {
        m_search->setMinimumWidth(qRound(BASE_SEARCH_MIN_WIDTH * scale));
        m_search->setTextMargins(qRound(26 * scale), 0, qRound(4 * scale), 0);
        m_search->setProperty("uiScale", scale); // read by SearchEdit's magnifier
    }
    for (auto it = m_categoryButtons.cbegin(); it != m_categoryButtons.cend(); ++it) {
        it.value()->setFixedSize(qRound(BASE_CATEGORY_WIDTH * scale),
                                 qRound(BASE_CATEGORY_HEIGHT * scale));
    }
}

void PickerWindow::setPanelSizeIndex(int index) {
    index = std::clamp(index, 0, static_cast<int>(PANEL_PRESETS.size()) - 1);
    m_panelSizeIndex = index;
    const double scale = PANEL_PRESETS.at(index).scale;
    applyStyles(scale);
    applyChromeScale(scale);
    m_canvas->setScale(scale);
    setFixedSize(qRound(BASE_PANEL_WIDTH * scale), qRound(BASE_PANEL_HEIGHT * scale));
    QSettings(QStringLiteral("PickMoji"), QStringLiteral("PickMoji"))
        .setValue(QStringLiteral("panelSizeLevel2"), index);
    emit panelSizeChanged(index);
}

void PickerWindow::cyclePanelSize(int delta) {
    setPanelSizeIndex(m_panelSizeIndex + delta);
}

QStringList PickerWindow::panelSizeLabels() {
    QStringList labels;
    labels.reserve(PANEL_PRESETS.size());
    for (const PanelPreset &preset : PANEL_PRESETS)
        labels << QString::fromLatin1(preset.label);
    return labels;
}

void PickerWindow::setShortcutHint(const QString &shortcut) {
    if (m_shortcutHint)
        m_shortcutHint->setText(shortcut);
}

void PickerWindow::scrollToSection(const QString &sectionId) {
    navigateTo(sectionId);
}

void PickerWindow::suspendAutoHideForInsertion() {
    m_autoHideSuppressed = true;
    m_autoHideArmed = false;
}

void PickerWindow::resumeAfterInsertion() {
    m_autoHideSuppressed = false;
    m_typingMode = false;
    m_autoHideArmed = false;
    if (isVisible())
        raise(); // keep on top without grabbing focus back from the target
}

void PickerWindow::hidePicker() {
    if (!isVisible())
        return;
    m_autoHideSuppressed = false;
    hide();
    emit hiddenByUser();
}

void PickerWindow::closeEvent(QCloseEvent *event) {
    event->ignore();
    hidePicker();
}

void PickerWindow::keyPressEvent(QKeyEvent *event) {
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
            cyclePanelSize(1);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Minus) {
            cyclePanelSize(-1);
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Escape) {
        hidePicker();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool PickerWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_search) {
        // Clicking Search is the cue to take focus (focus-on-demand). Let the
        // click continue so the caret lands normally.
        if (event->type() == QEvent::MouseButtonPress && !m_typingMode)
            emit searchFocusRequested();
        if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->modifiers().testFlag(Qt::ControlModifier)
                && (keyEvent->key() == Qt::Key_Plus || keyEvent->key() == Qt::Key_Equal)) {
                cyclePanelSize(1);
                return true;
            }
            if (keyEvent->modifiers().testFlag(Qt::ControlModifier)
                && keyEvent->key() == Qt::Key_Minus) {
                cyclePanelSize(-1);
                return true;
            }
            if (keyEvent->key() == Qt::Key_Escape) {
                hidePicker();
                return true;
            }
            if (keyEvent->key() == Qt::Key_Down) {
                m_canvas->selectFirst();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool PickerWindow::event(QEvent *event) {
    if (event->type() == QEvent::WindowDeactivate && m_autoHideArmed && !m_variantMenuOpen) {
        QTimer::singleShot(90, this, [this]() {
            if (isVisible() && !isActiveWindow())
                hidePicker();
        });
    }
    return QWidget::event(event);
}
