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

#include "console/sync_wizard.h"

#include "base/logging.h"
#include "console/book/bootstrap.h"
#include "console/book/flat_book.h"
#include "console/book/sync_key.h"
#include "proto/router_book.pb.h"
#include "qt_base/application.h"

#include <QComboBox>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace console {

namespace {

enum Step
{
    STEP_BOOK = 0,
    STEP_PREVIEW,
    STEP_CONFIRM,
    STEP_COUNT
};

} // namespace

//--------------------------------------------------------------------------------------------------
SyncWizard::SyncWizard(const client::RouterConfig& router_config,
                       const proto::address_book::ComputerGroup& local_root,
                       QWidget* parent)
    : QDialog(parent),
      router_config_(router_config),
      local_root_(local_root)
{
    buildUi();

    setStatus(tr("Connecting to the router..."));

    controller_ = std::make_unique<BookController>(
        router_config_, qt_base::Application::uiTaskRunner());
    controller_->start(this);
}

//--------------------------------------------------------------------------------------------------
SyncWizard::~SyncWizard() = default;

//--------------------------------------------------------------------------------------------------
void SyncWizard::buildUi()
{
    setWindowTitle(tr("Address Book Synchronization"));
    setMinimumSize(560, 420);

    pages_ = new QStackedWidget(this);

    // Step one: which book and the passphrase for it.
    QWidget* book_page = new QWidget(this);
    QVBoxLayout* book_layout = new QVBoxLayout(book_page);

    QLabel* book_hint = new QLabel(
        tr("Choose the shared address book and enter the passphrase agreed in your department.\n"
           "The passphrase is what the records are encrypted with. The router never learns it."),
        book_page);
    book_hint->setWordWrap(true);
    book_layout->addWidget(book_hint);

    QFormLayout* form = new QFormLayout();
    book_combo_ = new QComboBox(book_page);
    passphrase_edit_ = new QLineEdit(book_page);
    passphrase_edit_->setEchoMode(QLineEdit::Password);

    form->addRow(tr("Shared book:"), book_combo_);
    form->addRow(tr("Passphrase:"), passphrase_edit_);
    book_layout->addLayout(form);

    status_label_ = new QLabel(book_page);
    status_label_->setWordWrap(true);
    book_layout->addWidget(status_label_);

    // Shown only when the router has no shared book yet: somebody has to make the first one, and
    // that somebody is whoever is setting this up.
    create_button_ = new QPushButton(tr("Create a shared book..."), book_page);
    create_button_->setVisible(false);
    connect(create_button_, &QPushButton::clicked, this, &SyncWizard::onCreateBook);
    book_layout->addWidget(create_button_);

    book_layout->addStretch();

    pages_->addWidget(book_page);

    // Step two: what joining would do.
    QWidget* preview_page = new QWidget(this);
    QVBoxLayout* preview_layout = new QVBoxLayout(preview_page);

    QLabel* preview_hint = new QLabel(
        tr("Computers are recognized by the address they are reached at, so the same machine is "
           "found even where it is named differently. Nothing has been changed yet."),
        preview_page);
    preview_hint->setWordWrap(true);
    preview_layout->addWidget(preview_hint);

    preview_tree_ = new QTreeWidget(preview_page);
    preview_tree_->setColumnCount(3);
    preview_tree_->setHeaderLabels(
        QStringList() << tr("In this book") << tr("In the shared book") << tr("Address"));
    preview_tree_->setRootIsDecorated(true);
    preview_tree_->header()->setStretchLastSection(true);
    preview_layout->addWidget(preview_tree_);

    pages_->addWidget(preview_page);

    // Step three: confirmation.
    QWidget* confirm_page = new QWidget(this);
    QVBoxLayout* confirm_layout = new QVBoxLayout(confirm_page);

    summary_label_ = new QLabel(confirm_page);
    summary_label_->setWordWrap(true);
    summary_label_->setTextFormat(Qt::PlainText);
    confirm_layout->addWidget(summary_label_);
    confirm_layout->addStretch();

    pages_->addWidget(confirm_page);

    back_button_ = new QPushButton(tr("Back"), this);
    next_button_ = new QPushButton(tr("Next"), this);

    QDialogButtonBox* buttons = new QDialogButtonBox(this);
    buttons->addButton(back_button_, QDialogButtonBox::ActionRole);
    buttons->addButton(next_button_, QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);

    connect(back_button_, &QPushButton::clicked, this, &SyncWizard::onBack);
    connect(next_button_, &QPushButton::clicked, this, &SyncWizard::onNext);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(pages_);
    layout->addWidget(buttons);

    showStep(STEP_BOOK);
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::showStep(int step)
{
    step_ = step;
    pages_->setCurrentIndex(step);
    updateButtons();
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::updateButtons()
{
    back_button_->setEnabled(step_ > STEP_BOOK && !waiting_);

    const bool ready = !waiting_ && (step_ != STEP_BOOK || book_combo_->count() > 0);
    next_button_->setEnabled(ready);

    next_button_->setText(step_ == STEP_CONFIRM ? tr("Join") : tr("Next"));
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::setStatus(const QString& text, bool error)
{
    if (!status_label_)
        return;

    status_label_->setText(text);
    status_label_->setStyleSheet(error ? QStringLiteral("color: #b00020;") : QString());
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::onBack()
{
    if (step_ > STEP_BOOK)
        showStep(step_ - 1);
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::onNext()
{
    switch (step_)
    {
        case STEP_BOOK:
            if (!checkPassphrase())
                return;
            startPullForPreview();
            break;

        case STEP_PREVIEW:
            showStep(STEP_CONFIRM);
            break;

        case STEP_CONFIRM:
            accept();
            break;

        default:
            break;
    }
}

//--------------------------------------------------------------------------------------------------
bool SyncWizard::checkPassphrase()
{
    if (book_combo_->count() == 0)
        return false;

    const int index = book_combo_->currentIndex();

    book_guid_ = book_combo_->itemData(index, Qt::UserRole).toString();
    salt_ = book_combo_->itemData(index, Qt::UserRole + 1).toByteArray();
    verifier_ = book_combo_->itemData(index, Qt::UserRole + 2).toByteArray();
    passphrase_ = passphrase_edit_->text();

    if (passphrase_.isEmpty())
    {
        setStatus(tr("Enter the passphrase."), true);
        return false;
    }

    sync_key_ = deriveSyncKey(passphrase_.toStdString(), salt_.toStdString());
    if (sync_key_.empty())
    {
        setStatus(tr("The router did not send a usable salt for this book."), true);
        return false;
    }

    // Checked here rather than after the records arrive: a wrong passphrase would otherwise look
    // like a book full of damaged records, and the person would have no way of telling which it is.
    if (!checkKeyVerifier(sync_key_, verifier_.toStdString()))
    {
        setStatus(tr("The passphrase does not match this book."), true);
        return false;
    }

    setStatus(QString());
    return true;
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::startPullForPreview()
{
    waiting_ = true;
    updateButtons();

    setStatus(tr("Reading the shared book..."));

    received_.clear();
    received_count_ = 0;

    proto::BookPullRequest request;
    request.set_book_guid(book_guid_.toStdString());
    request.set_since_revision(0);

    controller_->requestPull(request);
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::buildPreview()
{
    // The records were opened as they arrived; here they are put back into a tree, because that
    // is what the matching works on and what a person recognizes.
    remote_root_.Clear();
    rebuildBook(received_, &remote_root_, nullptr);

    const BootstrapPlan plan = planBootstrap(local_root_, remote_root_);

    preview_tree_->clear();

    QTreeWidgetItem* matched = new QTreeWidgetItem(preview_tree_);
    matched->setText(0, tr("The same computers (%1)").arg(plan.matched.size()));
    matched->setFirstColumnSpanned(true);

    for (const BootstrapPair& pair : plan.matched)
    {
        QTreeWidgetItem* item = new QTreeWidgetItem(matched);
        item->setText(0, QString::fromStdString(pair.local_name));
        item->setText(1, QString::fromStdString(pair.remote_name));
        item->setText(2, QString::fromStdString(pair.address));

        if (pair.names_differ)
        {
            // Keeping one name means losing the other, which is the only thing here worth
            // reading carefully.
            item->setToolTip(1, tr("The names differ. The shared name will be used."));
            item->setForeground(1, QBrush(QColor(0xB0, 0x60, 0x00)));
        }
    }

    QTreeWidgetItem* added = new QTreeWidgetItem(preview_tree_);
    added->setText(0, tr("Only in this book, will be added (%1)").arg(plan.only_local.size()));
    added->setFirstColumnSpanned(true);

    QTreeWidgetItem* ambiguous = new QTreeWidgetItem(preview_tree_);
    ambiguous->setText(0, tr("Could not be decided, left as they are (%1)")
                       .arg(plan.ambiguous.size()));
    ambiguous->setFirstColumnSpanned(true);

    matched->setExpanded(true);

    summary_label_->setText(
        tr("Joining the book \"%1\".\n\n"
           "  %2 computers are already there and will be kept as one record.\n"
           "  %3 computers from this machine will be added for everybody.\n"
           "  %4 could not be decided and stay as they are.\n\n"
           "A copy of the current address book file is made before anything is written.\n"
           "Synchronization can be switched off later; the book stays as it is.")
        .arg(book_combo_->currentText())
        .arg(plan.matched.size())
        .arg(plan.only_local.size())
        .arg(plan.ambiguous.size()));

    waiting_ = false;
    setStatus(QString());
    showStep(STEP_PREVIEW);
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::onCreateBook()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("Create a Shared Book"), tr("Name of the shared book:"), QLineEdit::Normal,
        tr("Department"), &accepted).trimmed();

    if (!accepted || name.isEmpty())
        return;

    const QString passphrase = QInputDialog::getText(
        this, tr("Create a Shared Book"),
        tr("Passphrase for the book. Everybody who joins it enters this same passphrase, and the "
           "router never learns it. It cannot be recovered if it is lost."),
        QLineEdit::Password, QString(), &accepted);

    if (!accepted || passphrase.isEmpty())
        return;

    // The salt and the verifier are made here rather than on the router, which is the whole point:
    // the router stores what it cannot read.
    const std::string salt = createSyncSalt();
    const std::string key = deriveSyncKey(passphrase.toStdString(), salt);
    if (key.empty())
    {
        setStatus(tr("Unable to prepare the key for the book."), true);
        return;
    }

    const std::string verifier = createKeyVerifier(key);
    if (verifier.empty())
    {
        setStatus(tr("Unable to prepare the key for the book."), true);
        return;
    }

    pending_book_name_ = name;
    pending_salt_ = QByteArray::fromStdString(salt);
    pending_verifier_ = QByteArray::fromStdString(verifier);

    waiting_ = true;
    updateButtons();
    setStatus(tr("Creating the shared book..."));

    // Creating needs an administrator session, so it goes over a connection of its own.
    admin_controller_ = std::make_unique<BookController>(
        router_config_, qt_base::Application::uiTaskRunner(), BookController::Role::ADMIN);
    admin_controller_->start(this);
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::onBookCreated(const std::string& guid, const std::string& error)
{
    waiting_ = false;

    if (!error.empty())
    {
        if (error == "already_exists")
            setStatus(tr("A shared book with this name already exists."), true);
        else
            setStatus(tr("The router refused to create the book."), true);

        updateButtons();
        return;
    }

    LOG(LS_INFO) << "Shared book created: " << guid;

    setStatus(tr("The shared book was created. Enter the passphrase to join it."));

    // The administrator connection has done its one job.
    admin_controller_.reset();

    // And the list is asked for again, so the new book appears in it.
    controller_->requestBookList(1);
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::onBookConnected()
{
    // Both connections report here, and there is no telling them apart from the call itself, so
    // the one that is up is asked. Without that, the ordinary connection coming back - after the
    // router was restarted, say - would set the administrator one to work before it had finished
    // connecting, and the request would be dropped on the floor.
    if (admin_controller_ && admin_controller_->isConnected() && !pending_book_name_.isEmpty())
    {
        admin_controller_->requestCreateBook(pending_book_name_.toStdString(),
                                             pending_salt_.toStdString(),
                                             pending_verifier_.toStdString());
        return;
    }

    setStatus(tr("Reading the list of shared books..."));
    controller_->requestBookList(1);
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::onBookDisconnected()
{
    waiting_ = false;
    setStatus(tr("The connection to the router was lost."), true);
    updateButtons();
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::onBookAuthFailed()
{
    waiting_ = false;
    setStatus(tr("The router refused the account stored in this address book."), true);
    updateButtons();
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::onBookList(const proto::BookList& message)
{
    book_combo_->clear();

    for (int i = 0; i < message.book_size(); ++i)
    {
        const proto::BookInfo& book = message.book(i);

        book_combo_->addItem(QString::fromStdString(book.name()));
        book_combo_->setItemData(i, QString::fromStdString(book.guid()), Qt::UserRole);
        book_combo_->setItemData(i, QByteArray::fromStdString(book.sync_salt()), Qt::UserRole + 1);
        book_combo_->setItemData(i, QByteArray::fromStdString(book.key_verifier()),
                                 Qt::UserRole + 2);
    }

    const bool empty = (book_combo_->count() == 0);

    if (empty)
    {
        setStatus(tr("The router has no shared address books yet. The first one has to be "
                     "created; that needs an administrator account on the router."), false);
    }
    else
    {
        setStatus(QString());
    }

    create_button_->setVisible(empty);
    updateButtons();
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::onBookPull(const proto::BookPull& message)
{
    if (message.error_code() != proto::BOOK_ERROR_CODE_OK)
    {
        waiting_ = false;
        setStatus(tr("The router refused to send the book."), true);
        updateButtons();
        return;
    }

    for (int i = 0; i < message.entry_size(); ++i)
    {
        const proto::BookEntryData& entry = message.entry(i);

        // Counted whatever becomes of it: the next page is taken by moving along what was sent,
        // and a record passed over here still took its place in that.
        ++received_count_;

        if (entry.deleted())
            continue;

        std::string payload;
        if (!openPayload(sync_key_, entry.payload(), &payload))
            continue;

        FlatEntry flat;
        flat.guid = entry.guid();
        flat.parent_guid = entry.parent_guid();
        flat.kind = (entry.kind() == proto::BOOK_ENTRY_KIND_COMPUTER) ? FlatEntry::Kind::COMPUTER
                                                                     : FlatEntry::Kind::GROUP;
        flat.payload = std::move(payload);

        received_.emplace_back(std::move(flat));
    }

    if (message.has_more())
    {
        proto::BookPullRequest request;
        request.set_book_guid(book_guid_.toStdString());
        request.set_since_revision(0);
        request.set_offset(received_count_);

        controller_->requestPull(request);
        return;
    }

    buildPreview();
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::onBookPushResult(const proto::BookPushResult& /* message */)
{
    // The wizard never writes; joining is what the tab does once this dialog is accepted.
}

//--------------------------------------------------------------------------------------------------
void SyncWizard::onBookChanged(const proto::BookChanged& /* message */)
{
    // Somebody is editing while this is open. What was read is still a fair picture of what
    // joining would do, and the exchange that follows will pick up the difference anyway.
}

} // namespace console
