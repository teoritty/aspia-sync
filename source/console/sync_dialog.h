//
// Aspia Project
// Copyright (C) 2016-2024 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#ifndef CONSOLE_SYNC_DIALOG_H
#define CONSOLE_SYNC_DIALOG_H

#include "base/macros_magic.h"

#include <QDialog>
#include <QPointer>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace console {

class AddressBookTab;

// What is going on with the synchronization of one address book, and the few things a person can
// do about it.
//
// It is a window of its own rather than a tab in the book properties because most of what it holds
// is not a setting: it is state that changes while it is open - what is connected, what is waiting
// to be sent, what is waiting to be decided.
class SyncDialog final : public QDialog
{
    Q_OBJECT

public:
    SyncDialog(AddressBookTab* tab, QWidget* parent);
    ~SyncDialog() final = default;

private slots:
    void onSyncStatusChanged();
    void onStopSync();
    void onConflictSelectionChanged();
    void onKeepMine();
    void onTakeTheirs();

private:
    void buildUi();
    void updateStatus();
    void resolveSelected(bool keep_local);

    // The tab outlives the dialog in ordinary use, but a book can be closed from elsewhere while
    // this is open, so it is not held as a bare pointer.
    QPointer<AddressBookTab> tab_;

    QLabel* state_label_ = nullptr;
    QLabel* pending_label_ = nullptr;
    QTreeWidget* conflict_tree_ = nullptr;
    QPushButton* keep_mine_button_ = nullptr;
    QPushButton* take_theirs_button_ = nullptr;

    DISALLOW_COPY_AND_ASSIGN(SyncDialog);
};

} // namespace console

#endif // CONSOLE_SYNC_DIALOG_H
