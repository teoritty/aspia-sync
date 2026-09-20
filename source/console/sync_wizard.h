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

#ifndef CONSOLE_SYNC_WIZARD_H
#define CONSOLE_SYNC_WIZARD_H

#include "base/macros_magic.h"
#include "client/router_config.h"
#include "console/book/book_controller.h"
#include "console/book/flat_book.h"
#include "proto/address_book.pb.h"

#include <QDialog>

#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;

namespace console {

// Joining a book that the department already keeps.
//
// It is laid out as three steps rather than one screen because it is the one moment when a mistake
// spoils the book for everybody at once, and the steps are what make the person look at what is
// about to happen before it happens:
//
//   1. which book, and the passphrase for it. The passphrase is checked here, against a value
//      stored with the book, so a typo is caught before anything else is done;
//   2. what joining would do: which records on this machine are the same computers as the ones
//      already there, what this machine adds, and what could not be decided;
//   3. confirmation, with a copy of the book file made before anything is written.
//
// Nothing is applied before the last step.
class SyncWizard final
    : public QDialog,
      public BookController::Delegate
{
    Q_OBJECT

public:
    SyncWizard(const client::RouterConfig& router_config,
               const proto::address_book::ComputerGroup& local_root,
               QWidget* parent);
    ~SyncWizard() final;

    // Filled once the dialog is accepted.
    QString bookGuid() const { return book_guid_; }

    // Binary, and carried as such. A salt is 32 random bytes and a verifier is ciphertext; put
    // through QString they would be read as UTF-8, and whatever is not valid UTF-8 comes back as a
    // replacement character - different bytes, a different length, and a key nobody else has.
    QByteArray salt() const { return salt_; }
    QByteArray verifier() const { return verifier_; }

    QString passphrase() const { return passphrase_; }

protected:
    // BookController::Delegate implementation.
    void onBookConnected() final;
    void onBookDisconnected() final;
    void onBookAuthFailed() final;
    void onBookList(const proto::BookList& message) final;
    void onBookPull(const proto::BookPull& message) final;
    void onBookPushResult(const proto::BookPushResult& message) final;
    void onBookChanged(const proto::BookChanged& message) final;
    void onBookCreated(const std::string& guid, const std::string& error) final;

private slots:
    void onBack();
    void onNext();
    void onCreateBook();

private:
    void buildUi();
    void showStep(int step);
    void updateButtons();
    void setStatus(const QString& text, bool error = false);

    bool checkPassphrase();
    void startPullForPreview();
    void buildPreview();

    const client::RouterConfig router_config_;
    const proto::address_book::ComputerGroup local_root_;

    std::unique_ptr<BookController> controller_;

    // A second connection, made only while a book is being created: that needs an administrator
    // session, and the one the book is read through is an ordinary client.
    std::unique_ptr<BookController> admin_controller_;
    QString pending_book_name_;
    QByteArray pending_salt_;
    QByteArray pending_verifier_;

    QStackedWidget* pages_ = nullptr;
    QComboBox* book_combo_ = nullptr;
    QLineEdit* passphrase_edit_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTreeWidget* preview_tree_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QPushButton* create_button_ = nullptr;
    QPushButton* back_button_ = nullptr;
    QPushButton* next_button_ = nullptr;

    // The shared book as it is rebuilt from what the router sent, used to work out the plan.
    // Records are opened as they arrive rather than kept sealed: holding a pile of encrypted
    // records only to decrypt them all at the end buys nothing and keeps them around longer.
    proto::address_book::ComputerGroup remote_root_;
    std::vector<FlatEntry> received_;
    int64_t received_count_ = 0;

    QString book_guid_;
    QByteArray salt_;
    QByteArray verifier_;
    QString passphrase_;
    std::string sync_key_;

    int step_ = 0;
    bool waiting_ = false;

    DISALLOW_COPY_AND_ASSIGN(SyncWizard);
};

} // namespace console

#endif // CONSOLE_SYNC_WIZARD_H
