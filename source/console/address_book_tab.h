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

#ifndef CONSOLE_ADDRESS_BOOK_TAB_H
#define CONSOLE_ADDRESS_BOOK_TAB_H

#include "base/macros_magic.h"
#include "client/router_config.h"
#include "client/online_checker/online_checker.h"
#include "console/book/book_controller.h"
#include "console/book/book_sync.h"
#include "proto/address_book.pb.h"
#include "ui_address_book_tab.h"

#include <optional>
#include <memory>

namespace console {

class ComputerItem;

class AddressBookTab final
    : public QWidget,
      public client::OnlineChecker::Delegate,
      public BookController::Delegate,
      public BookSync::Sender,
      public BookSync::Observer
{
    Q_OBJECT

public:
    ~AddressBookTab() final;

    static AddressBookTab* createNew(QWidget* parent);
    static AddressBookTab* openFromFile(const QString& file_path, QWidget* parent);

    QString addressBookName() const;
    QString addressBookGuid() const;
    const QString& filePath() const { return file_path_; }
    ComputerItem* currentComputer() const;
    std::string displayName() const;
    proto::address_book::ComputerGroup* currentComputerGroup() const;
    proto::address_book::ComputerGroup* rootComputerGroup();
    void setChanged(bool changed);
    bool isChanged() const { return is_changed_; }

    AddressBookTab* duplicateTab() const;

    bool save();
    bool saveAs();
    void reloadAll();

    bool isRouterEnabled() const;
    std::optional<client::RouterConfig> routerConfig() const;

    //----------------------------------------------------------------------------------------------
    // Synchronization.
    //----------------------------------------------------------------------------------------------

    bool isSyncEnabled() const;
    QString syncBookGuid() const;

    // Turns synchronization on for this book and starts it. |passphrase| is what the shared key is
    // derived from; it is not written to the file.
    bool enableSync(const QString& book_guid, const QString& salt, const QString& verifier,
                    const QString& passphrase);

    // Turns it off. The book stays exactly as it is and becomes an ordinary local file again.
    void disableSync();

    // What to show in the status bar.
    struct SyncStatus
    {
        bool enabled = false;
        bool connected = false;
        bool stopped = false;
        int pending = 0;
        int conflicts = 0;
    };

    SyncStatus syncStatus() const;

    void retranslateUi();

public slots:
    void addComputerGroup();
    void addComputer();
    void copyComputer();
    void modifyAddressBook();
    void modifyComputerGroup();
    void modifyComputer();
    void removeComputerGroup();
    void removeComputer();
    void startOnlineChecker();
    void stopOnlineChecker();

signals:
    void sig_addressBookChanged(bool changed);
    void sig_syncStatusChanged();
    void sig_computerGroupActivated(bool activated, bool is_root);
    void sig_computerActivated(bool activated);
    void sig_computerGroupContextMenu(const QPoint& point, bool is_root);
    void sig_computerContextMenu(ComputerItem* comouter_item, const QPoint& point);
    void sig_computerDoubleClicked(const proto::address_book::Computer& computer);
    void sig_updateStateForComputers(bool started);

protected:
    // BookController::Delegate implementation.
    void onBookConnected() final;
    void onBookDisconnected() final;
    void onBookAuthFailed() final;
    void onBookList(const proto::BookList& message) final;
    void onBookPull(const proto::BookPull& message) final;
    void onBookPushResult(const proto::BookPushResult& message) final;
    void onBookChanged(const proto::BookChanged& message) final;

    // BookSync::Sender implementation.
    void sendPull(const proto::BookPullRequest& request) final;
    void sendPush(const proto::BookPushRequest& request) final;

    // BookSync::Observer implementation.
    void onBookUpdated() final;
    void onConflicts(const std::vector<std::string>& guids) final;
    void onSyncStopped(SyncEngine::PullOutcome::Status reason) final;
    void onInSync(int64_t revision) final;

    // ConsoleTab implementation.
    void showEvent(QShowEvent* event) final;
    void keyPressEvent(QKeyEvent* event) final;

    // client::OnlineChecker::Delegate implementation.
    void onOnlineCheckerResult(int computer_id, bool online) final;
    void onOnlineCheckerFinished() final;

private slots:
    void onGroupItemClicked(QTreeWidgetItem* item, int column);
    void onGroupContextMenu(const QPoint& point);
    void onGroupItemCollapsed(QTreeWidgetItem* item);
    void onGroupItemExpanded(QTreeWidgetItem* item);
    void onGroupItemDropped();
    void onComputerItemClicked(QTreeWidgetItem* item, int column);
    void onComputerContextMenu(const QPoint& point);
    void onComputerItemDoubleClicked(QTreeWidgetItem* item, int column);

private:
    AddressBookTab(const QString& file_path,
                   proto::address_book::File&& file,
                   proto::address_book::Data&& data,
                   std::string&& key,
                   QWidget* parent);

    QByteArray saveState();
    void restoreState(const QByteArray& state);
    void updateComputerList(ComputerGroupItem* computer_group);
    bool saveToFile(const QString& file_path);
    ComputerGroupItem* rootComputerGroupItem();

    static QString parentName(ComputerGroupItem* item);
    static void showOpenError(QWidget* parent, const QString& message);
    static void showSaveError(QWidget* parent, const QString& message);

    Ui::AddressBookTab ui;

    QString file_path_;
    std::string key_;

    proto::address_book::File file_;
    proto::address_book::Data data_;

    bool is_changed_ = false;

    std::unique_ptr<client::OnlineChecker> online_checker_;

    // Present only while the book is synchronized. Both are dropped when it is switched off, which
    // is also what stops the exchange.
    std::unique_ptr<BookController> book_controller_;
    std::unique_ptr<BookSync> book_sync_;

    bool sync_connected_ = false;
    bool sync_stopped_ = false;

    void startSyncIfEnabled();
    void autoSave();

    DISALLOW_COPY_AND_ASSIGN(AddressBookTab);
};

} // namespace console

#endif // CONSOLE_ADDRESS_BOOK_TAB_H
