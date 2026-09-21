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

#include "console/sync_dialog.h"

#include "base/logging.h"
#include "console/address_book_tab.h"
#include "console/theme.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace console {

//--------------------------------------------------------------------------------------------------
SyncDialog::SyncDialog(AddressBookTab* tab, QWidget* parent)
    : QDialog(parent),
      tab_(tab)
{
    buildUi();
    updateStatus();

    if (tab_)
    {
        connect(tab_, &AddressBookTab::sig_syncStatusChanged,
                this, &SyncDialog::onSyncStatusChanged);
    }
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::buildUi()
{
    setWindowTitle(tr("Address Book Synchronization"));
    setMinimumSize(520, 380);

    QTabWidget* tabs = new QTabWidget(this);

    // What is going on.
    QWidget* state_page = new QWidget(this);
    QVBoxLayout* state_layout = new QVBoxLayout(state_page);

    QFormLayout* form = new QFormLayout();
    state_label_ = new QLabel(state_page);

    // What is said here when it stops is a sentence, not a word, and it is the sentence that tells
    // the person what to do about it. Cut off at the edge of the window it would say nothing.
    state_label_->setWordWrap(true);

    pending_label_ = new QLabel(state_page);

    form->addRow(tr("State:"), state_label_);
    form->addRow(tr("Waiting to be sent:"), pending_label_);
    state_layout->addLayout(form);

    QLabel* hint = new QLabel(
        tr("Changes are sent as soon as they are made, and what colleagues change arrives on its "
           "own. Nothing has to be saved by hand."),
        state_page);
    hint->setWordWrap(true);
    state_layout->addWidget(hint);
    state_layout->addStretch();

    QPushButton* stop_button = new QPushButton(tr("Stop synchronizing this book"), state_page);
    connect(stop_button, &QPushButton::clicked, this, &SyncDialog::onStopSync);
    state_layout->addWidget(stop_button);

    tabs->addTab(state_page, tr("State"));

    // What is waiting for a person.
    QWidget* conflict_page = new QWidget(this);
    QVBoxLayout* conflict_layout = new QVBoxLayout(conflict_page);

    QLabel* conflict_hint = new QLabel(
        tr("These computers were changed here and by somebody else in the same field, so neither "
           "version can be taken without losing the other. Until one is chosen, what you see in "
           "the book is your own version."),
        conflict_page);
    conflict_hint->setWordWrap(true);
    conflict_layout->addWidget(conflict_hint);

    conflict_tree_ = new QTreeWidget(conflict_page);
    conflict_tree_->setColumnCount(1);
    conflict_tree_->setHeaderLabels(QStringList() << tr("Computer"));
    conflict_tree_->setRootIsDecorated(false);
    conflict_tree_->header()->setStretchLastSection(true);
    connect(conflict_tree_, &QTreeWidget::itemSelectionChanged,
            this, &SyncDialog::onConflictSelectionChanged);
    conflict_layout->addWidget(conflict_tree_);

    // One record at a time, and said in terms of whose version wins rather than of what the
    // machinery does with it.
    keep_mine_button_ = new QPushButton(tr("Keep my version"), conflict_page);
    take_theirs_button_ = new QPushButton(tr("Take their version"), conflict_page);

    connect(keep_mine_button_, &QPushButton::clicked, this, &SyncDialog::onKeepMine);
    connect(take_theirs_button_, &QPushButton::clicked, this, &SyncDialog::onTakeTheirs);

    QHBoxLayout* conflict_buttons = new QHBoxLayout();
    conflict_buttons->addStretch();
    conflict_buttons->addWidget(keep_mine_button_);
    conflict_buttons->addWidget(take_theirs_button_);
    conflict_layout->addLayout(conflict_buttons);

    tabs->addTab(conflict_page, tr("Conflicts"));

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::updateStatus()
{
    if (!tab_)
        return;

    const AddressBookTab::SyncStatus status = tab_->syncStatus();

    if (!status.enabled)
    {
        state_label_->setText(tr("Not synchronized"));
    }
    else if (status.stopped)
    {
        // The two things that stop it for good both need a person, and saying so plainly is the
        // point: the alternative is a console that looks like it is working and is not.
        state_label_->setText(tr("Stopped. The router was restored from a backup, or the "
                                 "passphrase no longer matches. Join the book again."));
        state_label_->setStyleSheet(QStringLiteral("color: %1;").arg(errorColor().name()));
    }
    else if (status.connected)
    {
        state_label_->setText(tr("Synchronized"));
        state_label_->setStyleSheet(QString());
    }
    else
    {
        state_label_->setText(tr("Offline. Changes are kept and will be sent when the router is "
                                 "reachable again."));
        state_label_->setStyleSheet(QString());
    }

    pending_label_->setText(QString::number(status.pending));

    conflict_tree_->clear();

    for (const std::string& guid : tab_->syncConflicts())
    {
        QTreeWidgetItem* item = new QTreeWidgetItem(conflict_tree_);
        item->setText(0, tab_->computerNameByGuid(QString::fromStdString(guid)));
        item->setData(0, Qt::UserRole, QString::fromStdString(guid));
    }

    onConflictSelectionChanged();
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::onConflictSelectionChanged()
{
    const bool has_selection = (conflict_tree_->currentItem() != nullptr);

    keep_mine_button_->setEnabled(has_selection);
    take_theirs_button_->setEnabled(has_selection);
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::onKeepMine()
{
    resolveSelected(true);
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::onTakeTheirs()
{
    resolveSelected(false);
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::resolveSelected(bool keep_local)
{
    if (!tab_)
        return;

    QTreeWidgetItem* item = conflict_tree_->currentItem();
    if (!item)
        return;

    const QString name = item->text(0);

    if (!keep_local)
    {
        // Their version replaces what is here, and what is here is gone once it does. Sending
        // one's own version can be undone by editing the record again; this cannot.
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, tr("Confirmation"),
            tr("Replace \"%1\" with the version your colleagues have?\n\n"
               "What you changed here will be lost.").arg(name),
            QMessageBox::Yes | QMessageBox::No);

        if (answer != QMessageBox::Yes)
            return;
    }

    if (!tab_->resolveSyncConflict(item->data(0, Qt::UserRole).toString(), keep_local))
    {
        LOG(LS_ERROR) << "Unable to resolve the conflict for " << name.toStdString();
        return;
    }

    updateStatus();
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::onSyncStatusChanged()
{
    updateStatus();
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::onStopSync()
{
    if (!tab_)
        return;

    const AddressBookTab::SyncStatus status = tab_->syncStatus();

    QString question = tr("Stop synchronizing this address book?\n\n"
                          "The book stays exactly as it is and becomes an ordinary local file "
                          "again. Changes made here will no longer reach your colleagues, and "
                          "theirs will no longer reach you.");

    if (status.pending > 0)
    {
        // Leaving now would strand them, and that is worth saying before rather than after.
        question += tr("\n\n%1 change(s) have not been sent yet and will stay on this machine "
                       "only.").arg(status.pending);
    }

    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Confirmation"), question, QMessageBox::Yes | QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    tab_->disableSync();
    updateStatus();
}

} // namespace console
