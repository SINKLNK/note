#pragma once

#include "ProcessEntry.h"

#include <QDialog>

class ProcessDetailsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProcessDetailsDialog(const ProcessEntry &entry, QWidget *parent = nullptr);
};
