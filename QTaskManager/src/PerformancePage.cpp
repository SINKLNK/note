#include "PerformancePage.h"

#include "PerfGraph.h"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLocale>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace
{
// Width over height of the charts that follow the window height, measured off
// the reference mockup. Each chart keeps its own ratio.
constexpr double kCpuGridAspect = 1048.0 / 714.0;
constexpr double kMemoryAspect = 1042.0 / 397.0;
constexpr double kGpu3dAspect = 725.0 / 309.0;
constexpr double kGpuDecodeAspect = 725.0 / 312.0;

constexpr int kMargin = 16;       // around the whole board
constexpr int kGap = 10;          // between cards
constexpr int kPadding = 12;      // inside a card
constexpr int kTitleGap = 2;      // card title to the caption under it
constexpr int kCaptionGap = 4;    // caption to the chart it labels
constexpr int kSectionGap = 8;    // chart to the next caption
constexpr int kStatsGap = 10;     // last chart to the numbers under it
constexpr int kMinChartHeight = 40;
constexpr int kGpuMemoryGraphHeight = 48;
constexpr int kCompositionHeight = 20;
constexpr int kCoreSpacing = 4;
constexpr int kAudioGap = 6;
constexpr int kAudioPadding = 10;
constexpr qreal kCardRadius = 8.0;

const QColor kBoardBackground(234, 238, 243);
const QColor kCardBackground(251, 252, 253);
const QColor kCardBorder(218, 223, 230);
const QColor kCpuColor(17, 125, 187);
const QColor kMemoryColor(76, 110, 196);
const QColor kGpuColor(146, 72, 202);

QLabel *makeLabel(const QString &text, const char *objectName, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QLatin1String(objectName));
    return label;
}

// A left/right pair on one line: a card title and the device name, or a
// chart caption and the chart's scale.
QWidget *makeRow(QWidget *parent, QLabel *left, QLabel *right)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    left->setParent(row);
    right->setParent(row);
    layout->addWidget(left, 0, Qt::AlignBottom);
    layout->addStretch(1);
    layout->addWidget(right, 0, Qt::AlignBottom);
    return row;
}

// A grey caption over a large value. Returns the value label so it can be
// updated later.
QLabel *addStat(QWidget *parent, QGridLayout *grid, int row, int column,
                const QString &caption, const QString &value)
{
    auto *block = new QWidget(parent);
    auto *layout = new QVBoxLayout(block);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(makeLabel(caption, "PerfStatCaption", block));
    auto *valueLabel = makeLabel(value, "PerfStatValue", block);
    layout->addWidget(valueLabel);
    grid->addWidget(block, row, column, Qt::AlignLeft | Qt::AlignTop);
    return valueLabel;
}

QGridLayout *makeStatsGrid(QWidget *owner, int horizontalSpacing)
{
    auto *grid = new QGridLayout(owner);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(horizontalSpacing);
    grid->setVerticalSpacing(8);
    return grid;
}

QLabel *makeIcon(const QString &path, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setPixmap(QIcon(path).pixmap(QSize(20, 20), parent->devicePixelRatioF()));
    return label;
}

void paintCard(QPainter &painter, const QPainterPath &shape)
{
    // Two faint copies shifted down stand in for a drop shadow.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 9));
    painter.drawPath(shape.translated(0, 2));
    painter.setBrush(QColor(0, 0, 0, 7));
    painter.drawPath(shape.translated(0, 1));

    painter.setPen(QPen(kCardBorder, 1));
    painter.setBrush(kCardBackground);
    painter.drawPath(shape);
}

QPainterPath roundedRect(const QRect &rect)
{
    QPainterPath path;
    path.addRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), kCardRadius, kCardRadius);
    return path;
}
} // namespace

// One small chart per logical processor, arranged in a grid as close to
// square as the count allows.
class CoreGrid : public QWidget
{
public:
    explicit CoreGrid(QWidget *parent = nullptr)
        : QWidget(parent)
    {
    }

    void setCoreCount(int count)
    {
        qDeleteAll(m_cores);
        m_cores.clear();
        for (int i = 0; i < count; ++i) {
            auto *core = new PerfGraph(kCpuColor, this);
            core->setGrid(6, 5);
            core->show();
            m_cores.append(core);
        }

        // Fewest wasted cells first, then the shape closest to square; ties
        // go to the wider grid.
        m_columns = 1;
        double bestScore = 1e9;
        for (int columns = 1; columns <= qMax(1, count); ++columns) {
            const int rows = (count + columns - 1) / columns;
            const double score = std::abs(std::log(double(rows) / columns))
                                 + 0.5 * (rows * columns - count) / qMax(1, count);
            if (score <= bestScore) {
                bestScore = score;
                m_columns = columns;
            }
        }
        arrange();
    }

    int count() const { return static_cast<int>(m_cores.size()); }
    PerfGraph *core(int index) const { return m_cores.at(index); }

protected:
    void resizeEvent(QResizeEvent *) override { arrange(); }

private:
    QVector<PerfGraph *> m_cores;
    int m_columns = 1;

    void arrange()
    {
        if (m_cores.isEmpty()) {
            return;
        }
        const int rows = (count() + m_columns - 1) / m_columns;
        for (int i = 0; i < count(); ++i) {
            const int column = i % m_columns;
            const int row = i / m_columns;
            // Edges from integer division, so cells tile the area without a
            // sliver left over at the right or bottom.
            const int left = column * (width() + kCoreSpacing) / m_columns;
            const int right = (column + 1) * (width() + kCoreSpacing) / m_columns - kCoreSpacing;
            const int top = row * (height() + kCoreSpacing) / rows;
            const int bottom = (row + 1) * (height() + kCoreSpacing) / rows - kCoreSpacing;
            m_cores[i]->setGeometry(left, top, right - left, bottom - top);
        }
    }
};

// Physical memory as one bar: in use (filled), modified (a solid sliver),
// standby (empty) and free (the narrow tail past the last divider).
class MemoryCompositionBar : public QWidget
{
public:
    using QWidget::QWidget;

    void setComposition(double inUse, double modified, double standby)
    {
        m_inUse = inUse;
        m_modified = modified;
        m_standby = standby;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::white);

        const QRect frame = rect().adjusted(0, 0, -1, -1);
        const int span = frame.width();
        const int inUseEnd = frame.left() + qRound(span * m_inUse);
        const int modifiedEnd = inUseEnd + qMax(2, qRound(span * m_modified));
        const int standbyEnd = qMin(frame.right(), modifiedEnd + qRound(span * m_standby));

        QColor fill = kMemoryColor;
        fill.setAlpha(70);
        painter.fillRect(QRect(frame.left(), frame.top(), inUseEnd - frame.left(), frame.height()), fill);
        painter.fillRect(QRect(inUseEnd, frame.top(), modifiedEnd - inUseEnd, frame.height()), kMemoryColor);

        QColor border = kMemoryColor;
        border.setAlpha(150);
        painter.setPen(border);
        painter.drawLine(standbyEnd, frame.top(), standbyEnd, frame.bottom());
        painter.drawRect(frame);
    }

private:
    double m_inUse = 0.0;
    double m_modified = 0.0;
    double m_standby = 0.0;
};

// Everything on the page. Child widgets are positioned by hand in arrange();
// the cards behind them are painted here rather than being widgets, because
// the memory card is L-shaped and reaches under the GPU column.
class PerformanceBoard : public QWidget
{
public:
    explicit PerformanceBoard(QWidget *parent = nullptr);

    // The smallest width the board takes for a given height, with every chart
    // at its own aspect ratio. Narrower than that the page has to scroll;
    // wider, the cards are stretched sideways to fill the viewport.
    QSize contentSizeFor(int height) const { return compute(height, 0).content; }
    void arrange(const QSize &viewport);

    CoreGrid *cores = nullptr;
    PerfGraph *memoryGraph = nullptr;
    MemoryCompositionBar *composition = nullptr;
    PerfGraph *gpu3d = nullptr;
    PerfGraph *gpuDecode = nullptr;
    PerfGraph *gpuDedicated = nullptr;
    PerfGraph *gpuShared = nullptr;

    QLabel *cpuName = nullptr;
    QLabel *cpuUsageValue = nullptr;
    QLabel *cpuSpeedValue = nullptr;
    QLabel *processesValue = nullptr;
    QLabel *threadsValue = nullptr;
    QLabel *handlesValue = nullptr;
    QLabel *uptimeValue = nullptr;

    QLabel *memoryDevice = nullptr;
    QLabel *memoryTotal = nullptr;
    QLabel *memoryInUseValue = nullptr;
    QLabel *memoryAvailableValue = nullptr;
    QLabel *memoryCommittedValue = nullptr;
    QLabel *memoryCachedValue = nullptr;
    QLabel *pagedPoolValue = nullptr;
    QLabel *nonPagedPoolValue = nullptr;

    QLabel *gpuName = nullptr;
    QLabel *gpuDedicatedLimit = nullptr;
    QLabel *gpuSharedLimit = nullptr;
    QLabel *gpuUsageValue = nullptr;
    QLabel *gpuDedicatedValue = nullptr;
    QLabel *gpuMemoryValue = nullptr;
    QLabel *gpuSharedValue = nullptr;
    QLabel *gpuTemperatureValue = nullptr;

    QLabel *volumeValue = nullptr;
    QSlider *volumeSlider = nullptr;
    QComboBox *outputCombo = nullptr;
    QComboBox *inputCombo = nullptr;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Geometry
    {
        QVector<QPair<QWidget *, QRect>> placements;
        QVector<QRect> cards;
        QRect memoryCard;
        QRect memoryExtension;
        QSize content;
    };

    Geometry compute(int height, int viewportWidth) const;

    Geometry m_geometry;
    QPoint m_offset;

    QWidget *m_cpuHeader = nullptr;
    QWidget *m_cpuCaption = nullptr;
    QWidget *m_cpuStats = nullptr;
    QWidget *m_memoryHeader = nullptr;
    QWidget *m_memoryCaption = nullptr;
    QWidget *m_memoryAxis = nullptr;
    QWidget *m_compositionCaption = nullptr;
    QWidget *m_memoryStats = nullptr;
    QWidget *m_gpuHeader = nullptr;
    QWidget *m_gpu3dCaption = nullptr;
    QWidget *m_gpuDecodeCaption = nullptr;
    QWidget *m_gpuDedicatedCaption = nullptr;
    QWidget *m_gpuSharedCaption = nullptr;
    QWidget *m_gpuStats = nullptr;
    QVector<QWidget *> m_audioRows;
};

PerformanceBoard::PerformanceBoard(QWidget *parent)
    : QWidget(parent)
{
    // Everything that comes from the system starts as a dash until the
    // monitor's first report arrives, a second or so after start-up.
    const QString none = QStringLiteral("—");

    // --- CPU ---------------------------------------------------------------
    cpuName = makeLabel(none, "PerfDevice", this);
    m_cpuHeader = makeRow(this, makeLabel(QStringLiteral("ЦП"), "PerfTitle", this), cpuName);
    m_cpuCaption = makeRow(this,
                           makeLabel(QStringLiteral("% использования за 60 секунд"), "PerfCaption", this),
                           makeLabel(QStringLiteral("100%"), "PerfCaption", this));
    cores = new CoreGrid(this);

    m_cpuStats = new QWidget(this);
    {
        // One row, three groups pushed apart: load, counts, uptime.
        auto *grid = makeStatsGrid(m_cpuStats, 20);
        cpuUsageValue = addStat(m_cpuStats, grid, 0, 0, QStringLiteral("Использование"), none);
        cpuSpeedValue = addStat(m_cpuStats, grid, 0, 1, QStringLiteral("Скорость"), none);
        grid->setColumnStretch(2, 1);
        processesValue = addStat(m_cpuStats, grid, 0, 3, QStringLiteral("Процессы"), none);
        threadsValue = addStat(m_cpuStats, grid, 0, 4, QStringLiteral("Потоки"), none);
        handlesValue = addStat(m_cpuStats, grid, 0, 5, QStringLiteral("Дескрипторы"), none);
        grid->setColumnStretch(6, 1);
        uptimeValue = addStat(m_cpuStats, grid, 0, 7, QStringLiteral("Время работы"), none);
    }

    // --- Memory ------------------------------------------------------------
    memoryDevice = makeLabel(none, "PerfDevice", this);
    m_memoryHeader = makeRow(this, makeLabel(QStringLiteral("Память"), "PerfTitle", this), memoryDevice);
    memoryTotal = makeLabel(none, "PerfCaption", this);
    m_memoryCaption = makeRow(this, makeLabel(QStringLiteral("Использование памяти"), "PerfCaption", this),
                              memoryTotal);
    memoryGraph = new PerfGraph(kMemoryColor, this);
    memoryGraph->setGrid(10, 10);
    memoryGraph->setHoverUnit(QStringLiteral("ГБ"));
    m_memoryAxis = makeRow(this,
                           makeLabel(QStringLiteral("60 секунд"), "PerfCaption", this),
                           makeLabel(QStringLiteral("0"), "PerfCaption", this));
    m_compositionCaption = makeLabel(QStringLiteral("Структура памяти"), "PerfCaption", this);
    composition = new MemoryCompositionBar(this);

    m_memoryStats = new QWidget(this);
    {
        auto *grid = makeStatsGrid(m_memoryStats, 24);
        memoryInUseValue = addStat(m_memoryStats, grid, 0, 0, QStringLiteral("Используется (сжатая)"), none);
        memoryAvailableValue = addStat(m_memoryStats, grid, 0, 1, QStringLiteral("Доступно"), none);
        memoryCommittedValue = addStat(m_memoryStats, grid, 1, 0, QStringLiteral("Выделено"), none);
        memoryCachedValue = addStat(m_memoryStats, grid, 1, 1, QStringLiteral("Кэшировано"), none);
        pagedPoolValue = addStat(m_memoryStats, grid, 2, 0, QStringLiteral("Выгружаемый пул"), none);
        nonPagedPoolValue = addStat(m_memoryStats, grid, 2, 1, QStringLiteral("Невыгружаемый пул"), none);
        grid->setColumnStretch(2, 1);
    }

    // --- GPU ---------------------------------------------------------------
    gpuName = makeLabel(none, "PerfDevice", this);
    m_gpuHeader = makeRow(this, makeLabel(QStringLiteral("GPU"), "PerfTitle", this), gpuName);
    m_gpu3dCaption = makeRow(this,
                             makeLabel(QStringLiteral("3D"), "PerfCaption", this),
                             makeLabel(QStringLiteral("100%"), "PerfCaption", this));
    gpu3d = new PerfGraph(kGpuColor, this);
    gpu3d->setGrid(16, 8);
    m_gpuDecodeCaption = makeRow(this,
                                 makeLabel(QStringLiteral("Video Decode"), "PerfCaption", this),
                                 makeLabel(QStringLiteral("100%"), "PerfCaption", this));
    gpuDecode = new PerfGraph(kGpuColor, this);
    gpuDecode->setGrid(16, 8);
    gpuDedicatedLimit = makeLabel(none, "PerfCaption", this);
    m_gpuDedicatedCaption = makeRow(this,
                                    makeLabel(QStringLiteral("Выделенная память графического процессора"), "PerfCaption", this),
                                    gpuDedicatedLimit);
    gpuDedicated = new PerfGraph(kGpuColor, this);
    gpuDedicated->setGrid(4, 6);
    gpuDedicated->setHoverUnit(QStringLiteral("ГБ"));
    gpuSharedLimit = makeLabel(none, "PerfCaption", this);
    m_gpuSharedCaption = makeRow(this,
                                 makeLabel(QStringLiteral("Общая память графического процессора"), "PerfCaption", this),
                                 gpuSharedLimit);
    gpuShared = new PerfGraph(kGpuColor, this);
    gpuShared->setGrid(4, 6);
    gpuShared->setHoverUnit(QStringLiteral("ГБ"));

    m_gpuStats = new QWidget(this);
    {
        auto *grid = makeStatsGrid(m_gpuStats, 24);
        gpuUsageValue = addStat(m_gpuStats, grid, 0, 0, QStringLiteral("Использование"), none);
        gpuDedicatedValue = addStat(m_gpuStats, grid, 0, 1, QStringLiteral("Выделенная память графического процессора"), none);
        gpuMemoryValue = addStat(m_gpuStats, grid, 1, 0, QStringLiteral("Память графического процессора"), none);
        gpuSharedValue = addStat(m_gpuStats, grid, 1, 1, QStringLiteral("Общая память графического процессора"), none);
        gpuTemperatureValue = addStat(m_gpuStats, grid, 2, 1, QStringLiteral("Температура"), none);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
    }

    // --- Audio -------------------------------------------------------------
    {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        layout->addWidget(makeLabel(QStringLiteral("Громкость"), "PerfAudioLabel", row));
        layout->addWidget(makeIcon(QStringLiteral(":/icons/speaker.svg"), row));
        volumeValue = makeLabel(none, "PerfAudioValue", row);
        volumeValue->setMinimumWidth(volumeValue->fontMetrics().horizontalAdvance(QStringLiteral("100")) + 4);
        layout->addWidget(volumeValue);
        volumeSlider = new QSlider(Qt::Horizontal, row);
        volumeSlider->setRange(0, 100);
        volumeSlider->setEnabled(false);
        layout->addWidget(volumeSlider, 1);
        connect(volumeSlider, &QSlider::valueChanged, volumeValue, [this](int volume) {
            volumeValue->setText(QString::number(volume));
        });
        m_audioRows.append(row);
    }
    const auto addDeviceRow = [this](const QString &icon) {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(10);
        layout->addWidget(makeIcon(icon, row));
        auto *combo = new QComboBox(row);
        // Size by a fixed number of characters rather than the longest name,
        // so a device with a long name elides instead of widening the board.
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(24);
        combo->setEnabled(false);
        layout->addWidget(combo, 1);
        m_audioRows.append(row);
        return combo;
    };
    outputCombo = addDeviceRow(QStringLiteral(":/icons/speaker.svg"));
    inputCombo = addDeviceRow(QStringLiteral(":/icons/mic.svg"));
}

PerformanceBoard::Geometry PerformanceBoard::compute(int height, int viewportWidth) const
{
    // Hints depend on the fonts the page style sheet assigns, which only
    // reach the labels once they are polished.
    ensurePolished();

    const auto hintHeight = [](const QWidget *widget) { return widget->sizeHint().height(); };
    const auto hintWidth = [](const QWidget *widget) { return widget->sizeHint().width(); };
    const int inner = height - 2 * kMargin;

    // Left column: the CPU card over the memory card. Their two charts split
    // what the fixed rows leave in proportion to 1/aspect, which is exactly
    // the split at which both have the same width and keep their ratios.
    const int cpuFixed = 2 * kPadding + hintHeight(m_cpuHeader) + kTitleGap + hintHeight(m_cpuCaption)
                         + kCaptionGap + kStatsGap + hintHeight(m_cpuStats);
    const int memoryFixed = 2 * kPadding + hintHeight(m_memoryHeader) + kTitleGap + hintHeight(m_memoryCaption)
                            + kCaptionGap + kTitleGap + hintHeight(m_memoryAxis) + kSectionGap
                            + hintHeight(m_compositionCaption) + kCaptionGap + kCompositionHeight;
    const int leftStretch = qMax(0, inner - cpuFixed - kGap - memoryFixed);
    const double leftWidth = leftStretch / (1.0 / kCpuGridAspect + 1.0 / kMemoryAspect);
    const int coresIdeal = qRound(leftWidth / kCpuGridAspect);
    const int coresHeight = qMax(kMinChartHeight, coresIdeal);
    const int memoryHeight = qMax(kMinChartHeight, leftStretch - coresIdeal);

    // The text rows set a floor on the width; below it the charts get wider
    // than their ratio rather than clipping the numbers.
    const int leftMinimum = std::max({hintWidth(m_cpuHeader), hintWidth(m_cpuCaption), hintWidth(m_cpuStats),
                                  hintWidth(m_memoryHeader), hintWidth(m_memoryCaption), hintWidth(m_memoryAxis)});
    int leftContent = qMax(leftMinimum, qRound(leftWidth));
    int leftCard = leftContent + 2 * kPadding;

    // Right column: the GPU card over a fixed-height row holding the memory
    // card's extension and the audio cards.
    int audioRowHint = 0;
    int audioWidth = 0;
    for (const QWidget *row : m_audioRows) {
        audioRowHint = qMax(audioRowHint, hintHeight(row));
        audioWidth = qMax(audioWidth, hintWidth(row));
    }
    audioWidth += 2 * kAudioPadding;
    const int audioCount = static_cast<int>(m_audioRows.size());
    const int bottomRow = qMax(hintHeight(m_memoryStats) + 2 * kPadding,
                               audioCount * (audioRowHint + 2 * kAudioPadding) + (audioCount - 1) * kAudioGap);

    const int gpuFixed = 2 * kPadding + hintHeight(m_gpuHeader) + kTitleGap
                         + hintHeight(m_gpu3dCaption) + kCaptionGap + kSectionGap
                         + hintHeight(m_gpuDecodeCaption) + kCaptionGap + kSectionGap
                         + hintHeight(m_gpuDedicatedCaption) + kCaptionGap + kGpuMemoryGraphHeight + kSectionGap
                         + hintHeight(m_gpuSharedCaption) + kCaptionGap + kGpuMemoryGraphHeight + kStatsGap
                         + hintHeight(m_gpuStats);
    const int rightStretch = qMax(0, inner - gpuFixed - kGap - bottomRow);
    const double rightWidth = rightStretch / (1.0 / kGpu3dAspect + 1.0 / kGpuDecodeAspect);
    const int gpu3dIdeal = qRound(rightWidth / kGpu3dAspect);
    const int gpu3dHeight = qMax(kMinChartHeight, gpu3dIdeal);
    const int gpuDecodeHeight = qMax(kMinChartHeight, rightStretch - gpu3dIdeal);

    // The extension under the GPU card spans from the memory card's edge to
    // the audio cards, which works out to the GPU card's width minus theirs.
    const int rightMinimum = std::max({hintWidth(m_gpuHeader), hintWidth(m_gpuDedicatedCaption),
                                   hintWidth(m_gpuSharedCaption), hintWidth(m_gpuStats),
                                   audioWidth + hintWidth(m_memoryStats)});
    int rightContent = qMax(rightMinimum, qRound(rightWidth));
    int rightCard = rightContent + 2 * kPadding;

    // A viewport wider than the board needs gives the extra width to the two
    // columns in proportion to their size, so the charts stretch sideways and
    // lose their aspect ratio instead of leaving empty margins.
    const int extra = viewportWidth - (2 * kMargin + leftCard + kGap + rightCard);
    if (extra > 0) {
        const int leftExtra = extra * leftCard / (leftCard + rightCard);
        leftContent += leftExtra;
        leftCard += leftExtra;
        rightContent += extra - leftExtra;
        rightCard += extra - leftExtra;
    }

    Geometry geometry;
    struct Cursor
    {
        int x;
        int y;
        int width;
    };
    const auto put = [&geometry](Cursor &cursor, QWidget *widget, int height, int gapAfter) {
        geometry.placements.append({widget, QRect(cursor.x, cursor.y, cursor.width, height)});
        cursor.y += height + gapAfter;
    };

    const int leftX = kMargin;
    Cursor cpu{leftX + kPadding, kMargin + kPadding, leftContent};
    put(cpu, m_cpuHeader, hintHeight(m_cpuHeader), kTitleGap);
    put(cpu, m_cpuCaption, hintHeight(m_cpuCaption), kCaptionGap);
    put(cpu, cores, coresHeight, kStatsGap);
    put(cpu, m_cpuStats, hintHeight(m_cpuStats), kPadding);
    geometry.cards.append(QRect(leftX, kMargin, leftCard, cpu.y - kMargin));

    const int memoryTop = cpu.y + kGap;
    Cursor memory{leftX + kPadding, memoryTop + kPadding, leftContent};
    put(memory, m_memoryHeader, hintHeight(m_memoryHeader), kTitleGap);
    put(memory, m_memoryCaption, hintHeight(m_memoryCaption), kCaptionGap);
    put(memory, memoryGraph, memoryHeight, kTitleGap);
    put(memory, m_memoryAxis, hintHeight(m_memoryAxis), kSectionGap);
    put(memory, m_compositionCaption, hintHeight(m_compositionCaption), kCaptionGap);
    put(memory, composition, kCompositionHeight, kPadding);

    const int rightX = leftX + leftCard + kGap;
    Cursor gpu{rightX + kPadding, kMargin + kPadding, rightContent};
    put(gpu, m_gpuHeader, hintHeight(m_gpuHeader), kTitleGap);
    put(gpu, m_gpu3dCaption, hintHeight(m_gpu3dCaption), kCaptionGap);
    put(gpu, gpu3d, gpu3dHeight, kSectionGap);
    put(gpu, m_gpuDecodeCaption, hintHeight(m_gpuDecodeCaption), kCaptionGap);
    put(gpu, gpuDecode, gpuDecodeHeight, kSectionGap);
    put(gpu, m_gpuDedicatedCaption, hintHeight(m_gpuDedicatedCaption), kCaptionGap);
    put(gpu, gpuDedicated, kGpuMemoryGraphHeight, kSectionGap);
    put(gpu, m_gpuSharedCaption, hintHeight(m_gpuSharedCaption), kCaptionGap);
    put(gpu, gpuShared, kGpuMemoryGraphHeight, kStatsGap);
    put(gpu, m_gpuStats, hintHeight(m_gpuStats), kPadding);
    geometry.cards.append(QRect(rightX, kMargin, rightCard, gpu.y - kMargin));

    // Both columns normally end on the same line. They only disagree when a
    // chart hit its minimum height; then the shorter side keeps the slack.
    const int bottom = qMax(memory.y, gpu.y + kGap + bottomRow);
    const int rowTop = bottom - bottomRow;
    geometry.memoryCard = QRect(leftX, memoryTop, leftCard, bottom - memoryTop);

    const int audioX = rightX + rightCard - audioWidth;
    geometry.memoryExtension = QRect(leftX + leftCard, rowTop, audioX - kGap - (leftX + leftCard), bottomRow);
    geometry.placements.append({m_memoryStats,
                                QRect(geometry.memoryExtension.left() + kPadding, rowTop + kPadding,
                                      geometry.memoryExtension.width() - 2 * kPadding, hintHeight(m_memoryStats))});

    const int audioHeight = (bottomRow - (audioCount - 1) * kAudioGap) / qMax(1, audioCount);
    for (int i = 0; i < audioCount; ++i) {
        const int top = rowTop + i * (audioHeight + kAudioGap);
        // The last card absorbs the rounding so the stack ends flush.
        const int cardHeight = i == audioCount - 1 ? bottom - top : audioHeight;
        const QRect card(audioX, top, audioWidth, cardHeight);
        geometry.cards.append(card);
        geometry.placements.append({m_audioRows[i], card.adjusted(kAudioPadding, 0, -kAudioPadding, 0)});
    }

    geometry.content = QSize(rightX + rightCard + kMargin, bottom + kMargin);
    return geometry;
}

void PerformanceBoard::arrange(const QSize &viewport)
{
    m_geometry = compute(viewport.height(), viewport.width());

    const QSize size = m_geometry.content.expandedTo(viewport);
    m_offset = QPoint((size.width() - m_geometry.content.width()) / 2,
                      (size.height() - m_geometry.content.height()) / 2);
    setFixedSize(size);

    for (const auto &[widget, rect] : m_geometry.placements) {
        widget->setGeometry(rect.translated(m_offset));
    }
    update();
}

void PerformanceBoard::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), kBoardBackground);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(m_offset);

    for (const QRect &card : m_geometry.cards) {
        paintCard(painter, roundedRect(card));
    }

    // The extension is widened leftwards into the memory card so the union
    // merges them into one outline; its own left corners end up inside.
    if (m_geometry.memoryCard.isValid()) {
        QPainterPath shape = roundedRect(m_geometry.memoryCard);
        if (m_geometry.memoryExtension.width() > 0) {
            const int overlap = qRound(2 * kCardRadius);
            shape = shape.united(roundedRect(m_geometry.memoryExtension.adjusted(-overlap, 0, 0, 0)));
        }
        paintCard(painter, shape);
    }
}

namespace
{
constexpr double kMiB = 1024.0 * 1024.0;
constexpr double kGiB = 1024.0 * kMiB;

const QLocale &russian()
{
    static const QLocale locale(QLocale::Russian);
    return locale;
}

// Three significant digits at most, the way Task Manager prints sizes:
// "1,9", "61,3", "111".
QString formatNumber(double value)
{
    return russian().toString(value, 'f', value >= 100.0 ? 0 : 1);
}

QString formatSize(quint64 bytes)
{
    if (bytes < kGiB) {
        return QStringLiteral("%1 МБ").arg(qRound(bytes / kMiB));
    }
    return QStringLiteral("%1 ГБ").arg(formatNumber(bytes / kGiB));
}

// "used/total", both in the unit the total calls for.
QString formatPair(quint64 used, quint64 total)
{
    if (total < kGiB) {
        return QStringLiteral("%1/%2 МБ").arg(qRound(used / kMiB)).arg(qRound(total / kMiB));
    }
    return QStringLiteral("%1/%2 ГБ").arg(formatNumber(used / kGiB), formatNumber(total / kGiB));
}

QString formatUptime(quint64 milliseconds)
{
    const quint64 seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(seconds / 86400)
        .arg(seconds / 3600 % 24, 2, 10, QLatin1Char('0'))
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString formatPercent(double value)
{
    return QStringLiteral("%1%").arg(qRound(value));
}

void fillDevices(QComboBox *combo, const QVector<AudioDevices::Device> &devices, const QString &defaultId)
{
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (const AudioDevices::Device &device : devices) {
        combo->addItem(device.name, device.id);
    }
    if (devices.isEmpty()) {
        combo->addItem(QStringLiteral("Нет устройств"));
    }
    combo->setEnabled(!devices.isEmpty());
    combo->setCurrentIndex(qMax(0, combo->findData(defaultId)));
}
} // namespace

PerformancePage::PerformancePage(QWidget *parent)
    : QWidget(parent)
    , m_monitor(new PerformanceMonitor())
    , m_audio(std::make_unique<AudioDevices>())
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(false);
    m_board = new PerformanceBoard;
    m_scroll->setWidget(m_board);
    layout->addWidget(m_scroll);

    setStyleSheet(R"(
        QLabel#PerfTitle {
            font-size: 24px;
            font-weight: 300;
            color: #111111;
        }
        QLabel#PerfDevice {
            font-size: 14px;
            color: #111111;
        }
        QLabel#PerfCaption {
            font-size: 11px;
            color: #555555;
        }
        QLabel#PerfStatCaption {
            font-size: 12px;
            color: #666666;
        }
        QLabel#PerfStatValue {
            font-size: 18px;
            color: #111111;
        }
        QLabel#PerfAudioLabel,
        QLabel#PerfAudioValue {
            font-size: 13px;
            color: #111111;
        }
    )");

    // valueChanged also fires for the keyboard and the wheel; refreshAudio()
    // blocks it while it moves the slider itself, so this is only the user.
    connect(m_board->volumeSlider, &QSlider::valueChanged, this, [this](int volume) {
        m_audio->setVolume(volume);
    });
    const auto connectDevices = [this](QComboBox *combo, QString *knownDefault) {
        connect(combo, &QComboBox::activated, this, [this, combo, knownDefault](int index) {
            const QString id = combo->itemData(index).toString();
            if (!id.isEmpty()) {
                m_audio->setDefaultDevice(id);
            }
            // Force a rebuild: if the switch failed, the box has to snap back
            // to the device that is still the default.
            knownDefault->clear();
            refreshAudio();
        });
    };
    connectDevices(m_board->outputCombo, &m_defaultOutput);
    connectDevices(m_board->inputCombo, &m_defaultInput);

    m_audioTimer.setInterval(1000);
    connect(&m_audioTimer, &QTimer::timeout, this, &PerformancePage::refreshAudio);

    m_monitor->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::started, m_monitor, &PerformanceMonitor::start);
    connect(&m_workerThread, &QThread::finished, m_monitor, &QObject::deleteLater);
    connect(m_monitor, &PerformanceMonitor::systemInfoReady, this, &PerformancePage::applySystemInfo);
    connect(m_monitor, &PerformanceMonitor::sampleReady, this, &PerformancePage::applySample);
    m_workerThread.start();
}

PerformancePage::~PerformancePage()
{
    // Same shutdown as ProcessPage: leaving the event loop stops the timer,
    // and the deleteLater queued on finished destroys the monitor in-thread.
    m_workerThread.quit();
    m_workerThread.wait();
}

void PerformancePage::setProcessSnapshot(const QVector<ProcessEntry> &entries)
{
    // Shown with the next sample, a second later at most.
    m_compressedKnown = false;
    for (const ProcessEntry &entry : entries) {
        if (entry.name == QLatin1String("Memory Compression")) {
            m_compressedBytes = entry.residentRamBytes;
            m_compressedKnown = true;
            break;
        }
    }
}

void PerformancePage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateBoardLayout();
}

void PerformancePage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateBoardLayout();
    refreshAudio();
    m_audioTimer.start();
}

void PerformancePage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_audioTimer.stop();
}

void PerformancePage::updateBoardLayout()
{
    // The scroll bars are decided here rather than left to the scroll area:
    // a horizontal bar takes height, which narrows the board, which can make
    // the bar unnecessary again. Deciding once from the full height and then
    // laying out for what is left settles it in one pass.
    const int extent = m_scroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_scroll);
    QSize viewport = size();

    bool horizontal = m_board->contentSizeFor(viewport.height()).width() > viewport.width();
    if (horizontal) {
        viewport.rheight() -= extent;
    }

    // Only reachable when the charts are already at their minimum height.
    const QSize content = m_board->contentSizeFor(viewport.height());
    const bool vertical = content.height() > viewport.height();
    if (vertical) {
        viewport.rwidth() -= extent;
        horizontal = horizontal || content.width() > viewport.width();
    }

    m_scroll->setHorizontalScrollBarPolicy(horizontal ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(vertical ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
    m_board->arrange(viewport);
}

void PerformancePage::applySystemInfo(const SystemInfo &info)
{
    m_info = info;
    const QString none = QStringLiteral("—");

    m_board->cpuName->setText(info.cpuName.isEmpty() ? none : info.cpuName);
    m_board->cores->setCoreCount(info.logicalProcessors);

    QString memory = info.installedMemory ? formatSize(info.installedMemory) : none;
    if (!info.memoryType.isEmpty()) {
        memory += QLatin1Char(' ') + info.memoryType;
    }
    m_board->memoryDevice->setText(memory);

    if (info.gpuName.isEmpty()) {
        m_board->gpuName->setText(QStringLiteral("Нет видеоадаптера"));
    } else {
        m_board->gpuName->setText(info.gpuName);
        m_board->gpuDedicatedLimit->setText(formatSize(info.gpuDedicatedTotal));
        m_board->gpuSharedLimit->setText(formatSize(info.gpuSharedTotal));
    }
    m_board->gpuDedicated->setMaximum(info.gpuDedicatedTotal / kGiB);
    m_board->gpuShared->setMaximum(info.gpuSharedTotal / kGiB);
    // Task Manager leaves the temperature out when the driver has none.
    m_board->gpuTemperatureValue->parentWidget()->setVisible(info.gpuTemperatureAvailable);

    // The names and the temperature row change the sizes the layout is
    // built from.
    updateBoardLayout();
}

void PerformancePage::applySample(const PerformanceSample &sample)
{
    PerformanceBoard &board = *m_board;
    const QString none = QStringLiteral("—");

    // --- CPU ---
    const int cores = qMin(board.cores->count(), static_cast<int>(sample.coreUsage.size()));
    for (int i = 0; i < cores; ++i) {
        board.cores->core(i)->appendValue(sample.coreUsage[i]);
        if (i < sample.coreKernel.size()) {
            board.cores->core(i)->appendSecondaryValue(qMin(sample.coreKernel[i], sample.coreUsage[i]));
        }
    }
    board.cpuUsageValue->setText(formatPercent(sample.cpuUsage));
    board.cpuSpeedValue->setText(
        sample.cpuSpeedMHz > 0.0
            ? QStringLiteral("%1 ГГц").arg(russian().toString(sample.cpuSpeedMHz / 1000.0, 'f', 2))
            : none);
    board.processesValue->setText(QString::number(sample.processes));
    board.threadsValue->setText(QString::number(sample.threads));
    board.handlesValue->setText(QString::number(sample.handles));
    board.uptimeValue->setText(formatUptime(sample.uptimeMs));

    // --- Memory ---
    const quint64 total = sample.physicalTotal;
    const quint64 inUse = total > sample.physicalAvailable ? total - sample.physicalAvailable : 0;
    board.memoryTotal->setText(formatSize(total));
    board.memoryGraph->setMaximum(total / kGiB);
    board.memoryGraph->appendValue(inUse / kGiB);

    QString inUseText = formatSize(inUse);
    if (m_compressedKnown) {
        inUseText += QStringLiteral(" (%1)").arg(formatSize(m_compressedBytes));
    }
    board.memoryInUseValue->setText(inUseText);
    board.memoryAvailableValue->setText(formatSize(sample.physicalAvailable));
    board.memoryCommittedValue->setText(formatPair(sample.commitTotal, sample.commitLimit));
    board.memoryCachedValue->setText(formatSize(sample.standby + sample.modified));
    board.pagedPoolValue->setText(formatSize(sample.pagedPool));
    board.nonPagedPoolValue->setText(formatSize(sample.nonPagedPool));

    if (total > 0) {
        // The bar's first segment is what is on none of the page lists, so
        // unlike "Используется" above it leaves the modified pages out; they
        // get their own sliver, as in Task Manager.
        const quint64 lists = sample.modified + sample.standby + sample.free;
        const double active = total > lists ? double(total - lists) : 0.0;
        board.composition->setComposition(active / total, double(sample.modified) / total,
                                          double(sample.standby) / total);
    }

    // --- GPU ---
    if (!m_info.gpuName.isEmpty()) {
        board.gpu3d->appendValue(sample.gpu3d);
        board.gpuDecode->appendValue(sample.gpuVideoDecode);
        board.gpuDedicated->appendValue(sample.gpuDedicatedUsed / kGiB);
        board.gpuShared->appendValue(sample.gpuSharedUsed / kGiB);

        board.gpuUsageValue->setText(formatPercent(sample.gpuUsage));
        board.gpuDedicatedValue->setText(formatPair(sample.gpuDedicatedUsed, m_info.gpuDedicatedTotal));
        board.gpuSharedValue->setText(formatPair(sample.gpuSharedUsed, m_info.gpuSharedTotal));
        board.gpuMemoryValue->setText(formatPair(sample.gpuDedicatedUsed + sample.gpuSharedUsed,
                                                 m_info.gpuDedicatedTotal + m_info.gpuSharedTotal));
        board.gpuTemperatureValue->setText(
            sample.gpuTemperature >= 0.0 ? QStringLiteral("%1 °C").arg(qRound(sample.gpuTemperature)) : none);
    }
}

void PerformancePage::refreshAudio()
{
    QSlider *slider = m_board->volumeSlider;
    const int volume = m_audio->volume();
    slider->setEnabled(volume >= 0);
    if (volume < 0) {
        m_board->volumeValue->setText(QStringLiteral("—"));
    } else if (!slider->isSliderDown()) {
        // Follow changes made elsewhere -- the volume keys, the tray flyout --
        // but never pull the handle out from under a drag.
        const QSignalBlocker blocker(slider);
        slider->setValue(volume);
        m_board->volumeValue->setText(QString::number(volume));
    }

    // Rebuild a list only when something about it changed, so an open popup
    // is not torn down under the user once a second.
    const auto refresh = [this](AudioDevices::Flow flow, QComboBox *combo,
                                QVector<AudioDevices::Device> &known, QString &knownDefault) {
        const QVector<AudioDevices::Device> devices = m_audio->devices(flow);
        const QString defaultId = m_audio->defaultDeviceId(flow);
        if (devices == known && defaultId == knownDefault && combo->count() > 0) {
            return;
        }
        known = devices;
        knownDefault = defaultId;
        fillDevices(combo, devices, defaultId);
    };
    refresh(AudioDevices::Flow::Output, m_board->outputCombo, m_outputs, m_defaultOutput);
    refresh(AudioDevices::Flow::Input, m_board->inputCombo, m_inputs, m_defaultInput);
}
