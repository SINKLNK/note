#include "MainWindow.h"
#include "SettingsPage.h"
#include "MonitoringClient.h"
#include "ProcessModel.h"
#include "ProcessPage.h"
#include "TestSupport.h"
#include <QAbstractButton>
#include <QApplication>
#include <QFontDatabase>
#include <QButtonGroup>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QScrollArea>
#include <QScrollBar>
#include <QPaintEvent>
#include <QThread>
#include <QTreeView>

class PaintRegionRecorder : public QObject
{
public:
    QRegion painted;

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint) {
            painted += static_cast<QPaintEvent *>(event)->region();
        }
        return false;
    }
};

// Exercise the real view and its cached/button delegates: BackgroundRole alone
// can be correct while the style sheet paints over the lifecycle tint.
static void checkProcessRowBackgrounds(MainWindow &window, QApplication &app)
{
    auto *page = window.findChild<ProcessPage *>();
    CHECK(page);
    auto *view = page->findChild<QTreeView *>();
    CHECK(view);
    auto *model = new ProcessModel(view);
    model->setTreeMode(false);
    view->setModel(model);

    ProcessEntry stable;
    stable.pid = 100;
    stable.creationTimeTicks = 100;
    stable.name = QStringLiteral("stable.exe");
    ProcessEntry exited = stable;
    exited.pid = 200;
    exited.creationTimeTicks = 200;
    exited.name = QStringLiteral("exited.exe");
    ProcessEntry arrived = stable;
    arrived.pid = 300;
    arrived.creationTimeTicks = 300;
    arrived.name = QStringLiteral("arrived.exe");
    arrived.parentPid = stable.pid;
    ProcessEntry survivor = arrived;
    survivor.pid = 400;
    survivor.creationTimeTicks = 400;
    survivor.name = QStringLiteral("survivor.exe");
    model->setEntries({stable, exited});
    model->setEntries({stable, arrived, survivor});
    window.findChild<QButtonGroup *>()->button(0)->click();
    app.processEvents();
    view->clearSelection();
    view->setCurrentIndex({});

    const auto checkRow = [view, model](const ProcessEntry &entry, const QColor &expected,
                                       int tolerance = 0) {
        const QModelIndex index = model->indexForProcess(entry);
        CHECK(index.isValid());
        QImage image(view->viewport()->size() * view->devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(view->devicePixelRatioF());
        image.fill(Qt::white);
        view->viewport()->render(&image);
        for (int column = 0; column < model->columnCount(); ++column) {
            if (view->isColumnHidden(column)) continue;
            const QRect rect = view->visualRect(index.siblingAtColumn(column));
            const QPoint sample(rect.center().x(), rect.top() + 2);
            if (!view->viewport()->rect().contains(sample)) continue;
            const QPoint pixel = sample * image.devicePixelRatio();
            const QColor actual = image.pixelColor(pixel);
            // The native Windows selection/focus effect can lighten the tint.
            const bool matches = std::abs(actual.red() - expected.red()) <= tolerance
                                 && std::abs(actual.green() - expected.green()) <= tolerance
                                 && std::abs(actual.blue() - expected.blue()) <= tolerance;
            if (!matches) {
                std::cerr << entry.name.toStdString() << " column " << column
                          << " background " << actual.name().toStdString()
                          << ", expected " << expected.name().toStdString() << '\n';
            }
            CHECK(matches);
        }
    };
    checkRow(stable, QColor("#ffffff"));
    checkRow(arrived, QColor("#ddf3dd"));
    checkRow(exited, QColor("#ffdfdf"));

    // Selecting a highlighted process must not replace its lifecycle colour
    // with the ordinary blue selection, including the delegate's focus path.
    view->setFocus();
    for (const auto &entry : {arrived, exited}) {
        view->setCurrentIndex(model->indexForProcess(entry));
        checkRow(entry, entry.pid == arrived.pid ? QColor("#ddf3dd") : QColor("#ffdfdf"), 12);
    }
    view->clearSelection();
    view->setCurrentIndex({});

    model->setTreeMode(true);
    view->setRootIsDecorated(true);
    view->setItemsExpandable(true);
    view->expandAll();
    checkRow(arrived, QColor("#ddf3dd"));
    checkRow(exited, QColor("#ffdfdf"));
    QImage preview(view->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::white);
    view->viewport()->render(&preview);
    CHECK(preview.save("process-highlights-preview.png"));
    const QRect childRect = view->visualRect(model->indexForProcess(arrived));
    CHECK(preview.pixelColor(3, childRect.top() + 2) == QColor("#ddf3dd"));

    // A colour transition must repaint the indentation too. A full render
    // would hide stale pixels left outside Qt's ordinary cell dirty rects.
    app.processEvents();
    PaintRegionRecorder recorder;
    view->viewport()->installEventFilter(&recorder);
    model->setEntries({stable, survivor});
    app.processEvents();
    CHECK(recorder.painted.contains(QPoint(3, childRect.top() + 2)));
    view->viewport()->removeEventFilter(&recorder);
    checkRow(arrived, QColor("#ffdfdf"));

    // Hidden endpoint columns must not prevent repainting the visible row,
    // including the unused space when the stretch column is hidden too.
    for (int column : {ProcessModel::NameColumn, ProcessModel::SharedGpuMemoryColumn,
                       ProcessModel::CommandLineColumn}) {
        view->setColumnHidden(column, true);
    }
    model->setSortFrozen(true);
    app.processEvents();
    app.sendPostedEvents(nullptr, QEvent::UpdateRequest);
    app.processEvents();
    const QRect survivorRect = view->visualRect(
        model->indexForProcess(survivor).siblingAtColumn(ProcessModel::CpuColumn));
    recorder.painted = {};
    view->viewport()->installEventFilter(&recorder);
    survivor.cpuPercent = 1.0;
    model->setEntries({stable, survivor});
    app.processEvents();
    CHECK(recorder.painted.contains(QPoint(view->viewport()->width() - 3, survivorRect.top() + 2)));
    view->viewport()->removeEventFilter(&recorder);
    for (int column : {ProcessModel::NameColumn, ProcessModel::SharedGpuMemoryColumn,
                       ProcessModel::CommandLineColumn}) {
        view->setColumnHidden(column, false);
    }

    // An exited row keeps its last values until retirement; arrival colour
    // clears on the first snapshot after the same five-second interval.
    CHECK(model->entryForIndex(model->indexForProcess(exited)) == exited);
    QThread::msleep(5100);
    model->setEntries({stable, survivor});
    CHECK(!model->indexForProcess(exited).isValid());
    CHECK(!model->indexForProcess(arrived).isValid());
    checkRow(survivor, QColor("#ffffff"));
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    int fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
    CHECK(fontId >= 0);
    app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).first(), 10));
    QTemporaryDir settings;
    CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    app.setOrganizationName("QTaskManagerTests");
    app.setApplicationName("GuiTests");
    QSettings().setValue("monitoring/startupOfferDismissed", true);
    MainWindow window;
    window.findChild<MonitoringClient *>()->stop();
    auto *model = window.findChild<ProcessModel *>();
    CHECK(model);
    CHECK(model->columnTitle(ProcessModel::DiskRateColumn) == QStringLiteral("Файлы, MB/s"));
    auto fileTooltip = model->headerData(ProcessModel::DiskRateColumn, Qt::Horizontal, Qt::ToolTipRole).toString();
    CHECK(fileTooltip.contains(QStringLiteral("кэш")));
    CHECK(fileTooltip.contains(QStringLiteral("запрос")));
    auto *sidebar = window.findChild<QWidget *>("Sidebar");
    CHECK(sidebar);
    auto *group = window.findChild<QButtonGroup *>();
    CHECK(group);
    auto *gear = group->button(3);
    CHECK(gear);
    for (int height : {680, 960})
    {
        window.resize(1200, height);
        window.show();
        app.processEvents();
        int bottom = gear->mapTo(sidebar, QPoint(0, gear->height())).y();
        CHECK(bottom <= sidebar->height());
        CHECK(sidebar->height() - bottom < 20);
    }
    gear->click();
    app.processEvents();
    CHECK(window.findChild<QStackedWidget *>()->currentIndex() == 3);
    CHECK(!window.findChild<QWidget *>("HeaderPanel")->isVisible());
    auto *page = window.findChild<SettingsPage *>();
    CHECK(page);
    SettingsState state;
    state.service.state = monitoring::ServiceState::Running;
    state.service.automatic = true;
    state.connected = true;
    state.live.disk.state = monitoring::SourceState::Partial;
    state.live.network.state = monitoring::SourceState::Ready;
    state.diagnostics.attributionTotal.diskEvents = 100000;
    state.diagnostics.attributionTotal.networkEvents = 200000;
    for (auto &count : state.diagnostics.attributionTotal.issues)
        count = {1234, 3000000, 123456, 234567, 5};
    state.lastResult = QStringLiteral("Проверка отображения диагностики (тестовые данные).");
    page->setState(state);
    window.resize(1000, 680);
    app.processEvents();
    auto *scroll = page->findChild<QScrollArea *>();
    CHECK(scroll);
    CHECK(scroll->verticalScrollBar()->maximum() > 0);
    CHECK(scroll->horizontalScrollBar()->maximum() == 0);
    window.resize(1200, 960);
    app.processEvents();
    CHECK(window.grab().save("settings-preview.png"));
    checkProcessRowBackgrounds(window, app);
}
