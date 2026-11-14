#pragma once

#include "ProcessEntry.h"

class QWidget;

// Things the user does to a process that more than one page offers.
namespace ProcessActions
{
#ifdef Q_OS_WIN
// Open the process a row describes, with PROCESS_QUERY_LIMITED_INFORMATION
// added to `access` (a DWORD), or tell the user why not and return null. The
// result is a HANDLE the caller closes.
//
// A PID on its own is not an identity. The snapshot behind a row is up to a
// second old, a confirmation dialog adds however long the user took, so the
// target may have exited and Windows may have handed its PID to something
// else entirely. Verify against the creation timestamp we recorded before
// touching anything; skip the check only when we never managed to read that
// timestamp in the first place.
void *openSameProcess(QWidget *parent, const ProcessEntry &entry, unsigned long access);
#endif

// Ask, then terminate. True when the process was actually terminated.
bool terminateProcess(QWidget *parent, const ProcessEntry &entry);
} // namespace ProcessActions
