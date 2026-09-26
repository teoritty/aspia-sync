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
#include "console/book/book_history.h"
#include "console/theme.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace console {

namespace {

// Where a history row keeps what it stands for.
constexpr int kRevisionRole = Qt::UserRole;
constexpr int kGuidRole = Qt::UserRole + 1;

//--------------------------------------------------------------------------------------------------
// Which field changed, never what it changed to: a password on a screen is a password on the photo
// somebody takes of it.
QString fieldName(const std::string& field)
{
    if (field == "name")
        return SyncDialog::tr("name");
    if (field == "address")
        return SyncDialog::tr("address");
    if (field == "port")
        return SyncDialog::tr("port");
    if (field == "comment")
        return SyncDialog::tr("comment");
    if (field == "username")
        return SyncDialog::tr("user name");
    if (field == "password")
        return SyncDialog::tr("password");
    if (field == "session_type")
        return SyncDialog::tr("session type");
    if (field == "inherit")
        return SyncDialog::tr("inheritance");
    if (field == "session_config")
        return SyncDialog::tr("session settings");
    if (field == "group")
        return SyncDialog::tr("group");

    return QString::fromStdString(field);
}

//--------------------------------------------------------------------------------------------------
QString timeText(int64_t server_time)
{
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(server_time),
                              QLocale::ShortFormat);
}

} // namespace

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
        connect(tab_, &AddressBookTab::sig_historyChanged,
                this, &SyncDialog::onHistoryChanged);
    }

    onHistoryChanged();
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::buildUi()
{
    setWindowTitle(tr("Address Book Synchronization"));
    setMinimumSize(760, 480);

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
    tabs->addTab(buildHistoryPage(), tr("Journal"));

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(tr("Close"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

//--------------------------------------------------------------------------------------------------
QWidget* SyncDialog::buildHistoryPage()
{
    // Who changed what and when, and the way back. Kept by the router, so every console sees the
    // same history, including changes made while it was switched off.
    QWidget* page = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(page);

    history_label_ = new QLabel(page);
    history_label_->setWordWrap(true);
    layout->addWidget(history_label_);

    history_tree_ = new QTreeWidget(page);
    history_tree_->setColumnCount(4);
    history_tree_->setHeaderLabels(QStringList()
        << tr("When") << tr("Computer") << tr("Action") << tr("Records"));
    history_tree_->header()->setStretchLastSection(true);
    history_tree_->setColumnWidth(0, 130);
    history_tree_->setColumnWidth(1, 170);
    history_tree_->setColumnWidth(2, 170);
    connect(history_tree_, &QTreeWidget::itemSelectionChanged,
            this, &SyncDialog::onHistorySelectionChanged);
    layout->addWidget(history_tree_);

    load_more_button_ = new QPushButton(tr("Load older"), page);
    rollback_button_ = new QPushButton(tr("Roll back the book to before this"), page);
    restore_button_ = new QPushButton(tr("Undo this change of the record"), page);

    // Said on the buttons themselves what they do not do, since the difference is the whole point:
    // one puts back everything after the moment, the other one record and nothing else.
    rollback_button_->setToolTip(
        tr("Undoes this change and every change made to the book after it, by anybody."));
    restore_button_->setToolTip(
        tr("Puts this one record back the way it was before this change. Nothing else is "
           "touched."));

    connect(load_more_button_, &QPushButton::clicked, this, &SyncDialog::onLoadMoreHistory);
    connect(rollback_button_, &QPushButton::clicked, this, &SyncDialog::onRollbackBook);
    connect(restore_button_, &QPushButton::clicked, this, &SyncDialog::onRestoreRecord);

    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addWidget(load_more_button_);
    buttons->addStretch();
    buttons->addWidget(restore_button_);
    buttons->addWidget(rollback_button_);
    layout->addLayout(buttons);

    return page;
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::onHistoryChanged()
{
    if (!tab_)
        return;

    // Asked for the first time the router says it keeps a history, which is usually straight
    // away: the dialog is opened on a book that has already synchronized.
    if (!history_requested_ && tab_->historyDays() > 0 && tab_->syncStatus().connected)
    {
        history_requested_ = true;
        tab_->requestHistory(false);
    }

    updateHistory();

    if (pending_rollback_.has_value() && !tab_->isHistoryLoading())
    {
        const PendingRollback pending = *pending_rollback_;
        pending_rollback_.reset();
        rollbackTo(pending.to_revision, pending.guid, pending.what);
    }
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::updateHistory()
{
    if (!tab_)
        return;

    const HistoryJournal& journal = tab_->history();

    if (tab_->historyDays() <= 0)
    {
        history_label_->setText(
            tr("The router does not keep a history of this book. It can be switched on in the "
               "router configuration (BookHistoryDays)."));
    }
    else if (tab_->historyMaxChanges() > 0)
    {
        history_label_->setText(
            tr("The router keeps the history for %1 day(s), at most %2 changes. Changes are "
               "shown with the computer they were made from.")
                .arg(tab_->historyDays()).arg(tab_->historyMaxChanges()));
    }
    else
    {
        history_label_->setText(
            tr("The router keeps the history for %1 day(s). Changes are shown with the computer "
               "they were made from.").arg(tab_->historyDays()));
    }

    // Remembered by what it stands for, not by position: the list is rebuilt from the top when
    // something new arrives.
    int64_t selected_revision = -1;
    QString selected_guid;
    if (QTreeWidgetItem* current = history_tree_->currentItem())
    {
        selected_revision = current->data(0, kRevisionRole).toLongLong();
        selected_guid = current->data(0, kGuidRole).toString();
    }

    history_tree_->clear();
    QTreeWidgetItem* to_select = nullptr;

    for (const HistoryBatch& batch : journal.batches())
    {
        size_t created = 0;
        size_t changed = 0;
        size_t deleted = 0;

        QTreeWidgetItem* item = new QTreeWidgetItem(history_tree_);
        item->setData(0, kRevisionRole, static_cast<qlonglong>(batch.revision));

        QStringList names;

        for (const HistoryChange& change : batch.changes)
        {
            const ChangeDescription description = describeChange(change);

            QString name = QString::fromStdString(description.name);
            if (name.isEmpty())
                name = tr("(unnamed)");
            if (change.kind == FlatEntry::Kind::GROUP)
                name = tr("Group \"%1\"").arg(name);

            QString action;
            switch (description.action)
            {
                case ChangeDescription::Action::CREATED:
                    action = tr("Added");
                    ++created;
                    break;
                case ChangeDescription::Action::DELETED:
                    action = tr("Deleted");
                    ++deleted;
                    break;
                case ChangeDescription::Action::RESTORED:
                    action = tr("Restored");
                    ++created;
                    break;
                case ChangeDescription::Action::MOVED:
                    action = tr("Moved");
                    ++changed;
                    break;
                case ChangeDescription::Action::UNREADABLE:
                    action = tr("Cannot be read");
                    ++changed;
                    break;
                default:
                    action = tr("Changed");
                    ++changed;
                    break;
            }

            // Which fields, never their values: a password on a screen is a password on the photo
            // somebody takes of it.
            QStringList fields;
            for (const std::string& field : description.fields)
                fields << fieldName(field);

            QTreeWidgetItem* child = new QTreeWidgetItem(item);
            child->setData(0, kRevisionRole, static_cast<qlonglong>(batch.revision));
            child->setData(0, kGuidRole, QString::fromStdString(change.guid));
            child->setText(2, action);
            child->setText(3, fields.isEmpty()
                ? name : tr("%1 (%2)").arg(name, fields.join(QStringLiteral(", "))));

            if (names.size() < 3)
                names << name;

            if (batch.revision == selected_revision &&
                selected_guid == QString::fromStdString(change.guid))
            {
                to_select = child;
            }
        }

        if (batch.changes.size() > 3)
            names << tr("and %1 more").arg(batch.changes.size() - 3);

        QString who = QString::fromStdString(batch.modified_by);
        if (!batch.address.empty())
            who += QStringLiteral(" (%1)").arg(QString::fromStdString(batch.address));

        QStringList summary;
        if (batch.rollback_to != 0)
        {
            const HistoryBatch* target = journal.findBatch(batch.rollback_to);
            summary << (target ? tr("Rollback to %1").arg(timeText(target->server_time))
                               : tr("Rollback to revision %1").arg(batch.rollback_to));
        }
        if (created)
            summary << tr("added: %1").arg(created);
        if (changed)
            summary << tr("changed: %1").arg(changed);
        if (deleted)
            summary << tr("deleted: %1").arg(deleted);

        item->setText(0, timeText(batch.server_time));
        item->setText(1, who);
        item->setText(2, summary.join(QStringLiteral(", ")));
        item->setText(3, names.join(QStringLiteral(", ")));

        // Deletions stand out: they are what somebody opening this is most likely looking for.
        if (deleted)
            item->setForeground(2, errorColor());

        if (batch.revision == selected_revision && selected_guid.isEmpty())
            to_select = item;
    }

    if (to_select)
    {
        history_tree_->setCurrentItem(to_select);
        if (to_select->parent())
            to_select->parent()->setExpanded(true);
    }

    updateHistoryButtons();
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::updateHistoryButtons()
{
    const bool available = tab_ && tab_->historyDays() > 0 && tab_->syncStatus().connected;
    const bool loading = tab_ && tab_->isHistoryLoading();

    QTreeWidgetItem* current = history_tree_->currentItem();

    load_more_button_->setEnabled(available && !loading && tab_->history().hasMore());
    rollback_button_->setEnabled(available && !loading && current != nullptr);
    restore_button_->setEnabled(available && !loading && current != nullptr &&
                                !current->data(0, kGuidRole).toString().isEmpty());
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::onHistorySelectionChanged()
{
    updateHistoryButtons();
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::onLoadMoreHistory()
{
    if (!tab_)
        return;

    tab_->requestHistory(true);
    updateHistoryButtons();
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::onRollbackBook()
{
    QTreeWidgetItem* current = history_tree_->currentItem();
    if (!tab_ || !current)
        return;

    // A row of a record stands for its batch here: the book goes back to before that batch.
    QTreeWidgetItem* batch_item = current->parent() ? current->parent() : current;
    const int64_t revision = batch_item->data(0, kRevisionRole).toLongLong();

    const QString what = tr("before the change made on %1 from %2")
        .arg(batch_item->text(0), batch_item->text(1));

    rollbackTo(revision - 1, QString(), what);
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::onRestoreRecord()
{
    QTreeWidgetItem* current = history_tree_->currentItem();
    if (!tab_ || !current || !current->parent())
        return;

    const int64_t revision = current->data(0, kRevisionRole).toLongLong();
    const QString guid = current->data(0, kGuidRole).toString();

    const QString what = tr("\"%1\" before the change made on %2 from %3")
        .arg(current->text(3), current->parent()->text(0), current->parent()->text(1));

    rollbackTo(revision - 1, guid, what);
}

//--------------------------------------------------------------------------------------------------
void SyncDialog::rollbackTo(int64_t to_revision, const QString& guid, const QString& what)
{
    if (!tab_)
        return;

    RollbackCounts counts;
    const RollbackCheck check = tab_->checkRollback(to_revision, guid, &counts);

    switch (check)
    {
        case RollbackCheck::OK:
            break;

        case RollbackCheck::NOT_LOADED:
            // The older pages are needed to know what the book was then. Asked for, and this runs
            // again when they are here.
            pending_rollback_ = PendingRollback{ to_revision, guid, what };
            tab_->requestHistory(true);
            updateHistoryButtons();
            return;

        case RollbackCheck::TOO_OLD:
            QMessageBox::warning(this, tr("Rollback"),
                tr("The history kept by the router does not reach back that far."));
            return;

        case RollbackCheck::NOTHING_TO_DO:
            QMessageBox::information(this, tr("Rollback"),
                tr("Nothing has changed since then. There is nothing to roll back."));
            return;

        default:
            QMessageBox::warning(this, tr("Rollback"),
                tr("The book is not in step with the router yet: something is waiting to be sent "
                   "or decided, or colleagues' changes have not arrived. Wait until it is "
                   "synchronized and try again."));
            return;
    }

    const QString question = (guid.isEmpty()
        ? tr("Roll back the whole book to how it was %1?").arg(what)
        : tr("Put back %1?").arg(what)) +
        tr("\n\nRecords brought back: %1\nRecords removed: %2\nRecords changed back: %3")
            .arg(counts.restored).arg(counts.removed).arg(counts.changed) +
        tr("\n\nThe change reaches your colleagues like any other edit and is recorded in the "
           "journal, so it can be undone the same way. A copy of the book file is made first.");

    if (QMessageBox::question(this, tr("Rollback"), question,
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
    {
        return;
    }

    QString backup_path;
    const RollbackCheck result = tab_->rollback(to_revision, guid, &backup_path);

    if (result == RollbackCheck::NOT_LOADED)
    {
        pending_rollback_ = PendingRollback{ to_revision, guid, what };
        return;
    }

    if (result != RollbackCheck::OK)
    {
        QMessageBox::warning(this, tr("Rollback"),
            tr("The book could not be rolled back. It has not been changed."));
        return;
    }

    if (!backup_path.isEmpty())
    {
        QMessageBox::information(this, tr("Rollback"),
            tr("Done. The book as it was before the rollback is kept in:\n%1")
                .arg(backup_path));
    }
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

    if (history_tree_)
        updateHistoryButtons();
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

    // The connection may have come up only now, and the history is asked for once it has.
    if (!history_requested_)
        onHistoryChanged();
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

    // Nothing is left to show here, and what usually comes next is joining again - this book or
    // another one - so the window closes and the join wizard takes over.
    accept();
}

} // namespace console
