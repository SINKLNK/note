#include "ProcessDetailsDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

ProcessDetailsDialog::ProcessDetailsDialog(const ProcessEntry &entry, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Процесс: %1").arg(entry.name));
    resize(700, 280);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    auto mkWrap = [](const QString &text) {
        auto *label = new QLabel(text);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };

    form->addRow(QStringLiteral("Имя"), mkWrap(entry.name));
    form->addRow(QStringLiteral("PID"), mkWrap(QString::number(entry.pid)));
    form->addRow(QStringLiteral("Пользователь"), mkWrap(entry.userName));
    form->addRow(QStringLiteral("Путь"), mkWrap(entry.executablePath));
    form->addRow(QStringLiteral("Командная строка"), mkWrap(entry.commandLine));
    form->addRow(QStringLiteral("Статус"), mkWrap(QStringLiteral("Пока заглушка. Здесь потом можно сделать полноценное окно свойств процесса.")));

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}
