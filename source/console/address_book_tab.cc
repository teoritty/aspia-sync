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

#include "console/address_book_tab.h"

#include "base/logging.h"
#include "base/crypto/data_cryptor_chacha20_poly1305.h"
#include "base/crypto/data_cryptor_fake.h"
#include "base/crypto/password_hash.h"
#include "base/crypto/secure_memory.h"
#include "base/files/file_util.h"
#include "base/strings/unicode.h"
#include "client/online_checker/online_checker.h"
#include "console/address_book_dialog.h"
#include "console/book/entry_guid.h"
#include "console/book/flat_book.h"
#include "console/book/sync_key.h"
#include "console/computer_dialog.h"
#include "console/computer_factory.h"
#include "console/computer_group_dialog.h"
#include "console/computer_item.h"
#include "console/open_address_book_dialog.h"
#include "console/settings.h"
#include "proto/router_book.pb.h"
#include "qt_base/application.h"

#include <QApplication>
#include <QFileDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QTimer>

#include <functional>

namespace console {

namespace {

//--------------------------------------------------------------------------------------------------
void cleanupComputer(proto::address_book::Computer* computer)
{
    if (!computer)
        return;

    base::memZero(computer->mutable_name());
    base::memZero(computer->mutable_address());
    base::memZero(computer->mutable_username());
    base::memZero(computer->mutable_password());
    base::memZero(computer->mutable_comment());
}

//--------------------------------------------------------------------------------------------------
void cleanupComputerGroup(proto::address_book::ComputerGroup* computer_group)
{
    if (!computer_group)
        return;

    for (int i = 0; i < computer_group->computer_size(); ++i)
        cleanupComputer(computer_group->mutable_computer(i));

    for (int i = 0; i < computer_group->computer_group_size(); ++i)
    {
        proto::address_book::ComputerGroup* child_group =
            computer_group->mutable_computer_group(i);

        base::memZero(child_group->mutable_name());
        base::memZero(child_group->mutable_comment());

        cleanupComputerGroup(child_group);
    }
}

//--------------------------------------------------------------------------------------------------
void cleanupData(proto::address_book::Data* data)
{
    if (!data)
        return;

    cleanupComputerGroup(data->mutable_root_group());
}

//--------------------------------------------------------------------------------------------------
void cleanupFile(proto::address_book::File* file)
{
    if (!file)
        return;

    base::memZero(file->mutable_hashing_salt());
    base::memZero(file->mutable_data());
}

} // namespace

//--------------------------------------------------------------------------------------------------
AddressBookTab::AddressBookTab(const QString& file_path,
                               proto::address_book::File&& file,
                               proto::address_book::Data&& data,
                               std::string&& key,
                               QWidget* parent)
    : QWidget(parent),
      file_path_(file_path),
      key_(std::move(key)),
      file_(std::move(file)),
      data_(std::move(data))
{
    LOG(LS_INFO) << "Ctor";
    ui.setupUi(this);

    ui.tree_group->setComputerMimeType(ui.tree_computer->mimeType());

    Settings settings;
    restoreState(settings.addressBookState());

    reloadAll();

    connect(ui.tree_group, &ComputerGroupTree::itemSelectionChanged, this, [this]()
    {
        if (ui.tree_group->dragging())
            return;

        QList<QTreeWidgetItem*> items = ui.tree_group->selectedItems();
        if (!items.isEmpty())
            onGroupItemClicked(items.front(), 0);
    });

    connect(ui.tree_group, &ComputerGroupTree::itemClicked,
            this, &AddressBookTab::onGroupItemClicked);

    connect(ui.tree_group, &ComputerGroupTree::customContextMenuRequested,
            this, &AddressBookTab::onGroupContextMenu);

    connect(ui.tree_group, &ComputerGroupTree::itemCollapsed,
            this, &AddressBookTab::onGroupItemCollapsed);

    connect(ui.tree_group, &ComputerGroupTree::itemExpanded,
            this, &AddressBookTab::onGroupItemExpanded);

    connect(ui.tree_group, &ComputerGroupTree::sig_itemDropped,
            this, &AddressBookTab::onGroupItemDropped);

    connect(ui.tree_computer, &ComputerTree::itemClicked,
            this, &AddressBookTab::onComputerItemClicked);

    connect(ui.tree_computer, &ComputerTree::customContextMenuRequested,
            this, &AddressBookTab::onComputerContextMenu);

    connect(ui.tree_computer, &ComputerTree::itemDoubleClicked,
            this, &AddressBookTab::onComputerItemDoubleClicked);

    connect(ui.edit_search, &QLineEdit::textChanged,
            this, &AddressBookTab::onSearchTextChanged);

    // The folder of a record is worth saying only when the records come from more than one, which
    // is to say only while searching.
    //
    // The width is set before it is hidden because hiding remembers the width to put back later,
    // and a column that has never been given one remembers nothing: it would come back the moment
    // somebody searched, and be nought pixels wide.
    ui.tree_computer->setColumnWidth(ComputerItem::COLUMN_INDEX_FOLDER, kFolderColumnWidth);
    ui.tree_computer->setColumnHidden(ComputerItem::COLUMN_INDEX_FOLDER, true);

    // A book that was joined before stays joined. It is put off until the event loop runs so that
    // whoever made this tab has finished connecting to its signals first, and hears about the
    // state of the connection.
    QTimer::singleShot(0, this, &AddressBookTab::resumeSyncIfEnabled);
}

//--------------------------------------------------------------------------------------------------
AddressBookTab::~AddressBookTab()
{
    LOG(LS_INFO) << "Dtor";

    cleanupData(&data_);
    cleanupFile(&file_);

    base::memZero(&key_);

    Settings settings;
    settings.setAddressBookState(saveState());
}

//--------------------------------------------------------------------------------------------------
// static
AddressBookTab* AddressBookTab::createNew(QWidget* parent)
{
    LOG(LS_INFO) << "[ACTION] Create new";

    proto::address_book::File file;
    proto::address_book::Data data;
    std::string key;

    file.set_encryption_type(proto::address_book::ENCRYPTION_TYPE_NONE);

    AddressBookDialog dialog(parent, QString(), &file, &data, &key);
    if (dialog.exec() != QDialog::Accepted)
    {
        LOG(LS_INFO) << "[ACTION] Create new rejected by user";
        return nullptr;
    }

    LOG(LS_INFO) << "[ACTION] Create new accepted by user";

    AddressBookTab* tab = new AddressBookTab(
        QString(), std::move(file), std::move(data), std::move(key), parent);

    tab->setChanged(true);
    return tab;
}

//--------------------------------------------------------------------------------------------------
// static
AddressBookTab* AddressBookTab::openFromFile(const QString& file_path, QWidget* parent)
{
    LOG(LS_INFO) << "Open address book from file: '" << file_path.toStdString() << "'";

    if (file_path.isEmpty())
    {
        LOG(LS_ERROR) << "Empty file path";
        return nullptr;
    }

    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly))
    {
        LOG(LS_ERROR) << "Unable to open file: " << file.errorString().toStdString();
        showOpenError(parent, tr("Unable to open address book file \"%1\".").arg(file_path));
        return nullptr;
    }

    QByteArray buffer = file.readAll();
    if (buffer.isEmpty())
    {
        LOG(LS_ERROR) << "Unable to read address book file";
        showOpenError(parent, tr("Unable to read address book file \"%1\".").arg(file_path));
        return nullptr;
    }

    proto::address_book::File address_book_file;

    if (!address_book_file.ParseFromArray(buffer.constData(), buffer.size()))
    {
        LOG(LS_ERROR) << "Unable to parse address book file";
        showOpenError(parent,
                      tr("The address book file \"%1\" is corrupted or has an unknown format.")
                      .arg(file_path));
        return nullptr;
    }

    proto::address_book::Data address_book_data;

    std::unique_ptr<base::DataCryptor> cryptor;
    std::string key;

    switch (address_book_file.encryption_type())
    {
        case proto::address_book::ENCRYPTION_TYPE_NONE:
            cryptor = std::make_unique<base::DataCryptorFake>();
            break;

        case proto::address_book::ENCRYPTION_TYPE_CHACHA20_POLY1305:
        {
            OpenAddressBookDialog dialog(parent, file_path, address_book_file.encryption_type());
            if (dialog.exec() != QDialog::Accepted)
                return nullptr;

            key = base::PasswordHash::hash(
                base::PasswordHash::SCRYPT,
                dialog.password().toStdString(),
                address_book_file.hashing_salt());

            cryptor = std::make_unique<base::DataCryptorChaCha20Poly1305>(key);
        }
        break;

        default:
            LOG(LS_ERROR) << "Unexpected encryption type: " << address_book_file.encryption_type();
            break;
    }

    if (!cryptor)
    {
        LOG(LS_ERROR) << "Unsupported encryption type";
        showOpenError(parent, tr("The address book file is encrypted with an unsupported encryption type."));
        return nullptr;
    }

    std::string decrypted_data;
    if (!cryptor->decrypt(address_book_file.data(), &decrypted_data))
    {
        LOG(LS_ERROR) << "Unable to decrypt address book";
        showOpenError(parent, tr("Unable to decrypt the address book with the specified password."));
        return nullptr;
    }

    if (!address_book_data.ParseFromString(decrypted_data))
    {
        LOG(LS_ERROR) << "Unable to parse address book";
        showOpenError(parent, tr("The address book file is corrupted or has an unknown format."));
        return nullptr;
    }

    base::memZero(&decrypted_data);

    // Books written before records had an identity carry no guid. Every record gets one here, once,
    // and the book is marked as changed so that they reach the file: guids generated anew at every
    // open would be no identity at all.
    const size_t guids_assigned = ensureEntryGuids(address_book_data.mutable_root_group());
    if (guids_assigned)
        LOG(LS_INFO) << "Assigned identity to " << guids_assigned << " address book entries";

    AddressBookTab* tab = new AddressBookTab(file_path,
                                             std::move(address_book_file),
                                             std::move(address_book_data),
                                             std::move(key),
                                             parent);
    if (guids_assigned)
        tab->setChanged(true);

    return tab;
}

//--------------------------------------------------------------------------------------------------
QString AddressBookTab::addressBookName() const
{
    return QString::fromStdString(data_.root_group().name());
}

//--------------------------------------------------------------------------------------------------
QString AddressBookTab::addressBookGuid() const
{
    return QString::fromStdString(data_.guid());
}

//--------------------------------------------------------------------------------------------------
ComputerItem* AddressBookTab::currentComputer() const
{
    return dynamic_cast<ComputerItem*>(ui.tree_computer->currentItem());
}

//--------------------------------------------------------------------------------------------------
std::string AddressBookTab::displayName() const
{
    return data_.display_name();
}

//--------------------------------------------------------------------------------------------------
proto::address_book::ComputerGroup* AddressBookTab::currentComputerGroup() const
{
    ComputerGroupItem* current_item =
        dynamic_cast<ComputerGroupItem*>(ui.tree_group->currentItem());
    if (!current_item)
        return nullptr;

    return current_item->computerGroup();
}

//--------------------------------------------------------------------------------------------------
proto::address_book::ComputerGroup* AddressBookTab::rootComputerGroup()
{
    return data_.mutable_root_group();
}

//--------------------------------------------------------------------------------------------------
AddressBookTab* AddressBookTab::duplicateTab() const
{
    LOG(LS_INFO) << "[ACTION] Duplicate tab";
    return new AddressBookTab(filePath(),
                              proto::address_book::File(file_),
                              proto::address_book::Data(data_),
                              std::string(key_),
                              static_cast<QWidget*>(parent()));
}

//--------------------------------------------------------------------------------------------------
bool AddressBookTab::save()
{
    LOG(LS_INFO) << "[ACTION] Save";
    return saveToFile(file_path_);
}

//--------------------------------------------------------------------------------------------------
bool AddressBookTab::saveAs()
{
    LOG(LS_INFO) << "[ACTION] Save as";
    return saveToFile(QString());
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::reloadAll()
{
    LOG(LS_INFO) << "Reload address book";

    ui.tree_group->clear();
    ui.tree_computer->clear();

    ComputerGroupItem* group_item = new ComputerGroupItem(data_.mutable_root_group(), nullptr);

    ui.tree_group->addTopLevelItem(group_item);
    ui.tree_group->setCurrentItem(group_item);

    updateComputerList(group_item);

    group_item->setExpanded(group_item->IsExpanded());

    std::function<void(ComputerGroupItem*)> restore_child = [&](ComputerGroupItem* item)
    {
        for (int i = 0; i < item->childCount(); ++i)
        {
            ComputerGroupItem* child_item = dynamic_cast<ComputerGroupItem*>(item->child(i));
            if (child_item)
            {
                if (child_item->IsExpanded())
                    child_item->setExpanded(true);

                restore_child(child_item);
            }
        }
    };

    restore_child(group_item);

    ui.tree_group->sortItems(0, Qt::AscendingOrder);
}

//--------------------------------------------------------------------------------------------------
bool AddressBookTab::isRouterEnabled() const
{
    return data_.enable_router();
}

//--------------------------------------------------------------------------------------------------
std::optional<client::RouterConfig> AddressBookTab::routerConfig() const
{
    if (!data_.enable_router())
        return std::nullopt;

    const proto::address_book::Router& router = data_.router();
    client::RouterConfig router_config;

    router_config.address  = base::utf16FromUtf8(router.address());
    router_config.port     = static_cast<uint16_t>(router.port());
    router_config.username = base::utf16FromUtf8(router.username());
    router_config.password = base::utf16FromUtf8(router.password());

    return std::move(router_config);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::addComputerGroup()
{
    LOG(LS_INFO) << "[ACTION] Add computer group";
    stopOnlineChecker();

    ComputerGroupItem* parent_item =
        dynamic_cast<ComputerGroupItem*>(ui.tree_group->currentItem());
    if (!parent_item)
    {
        LOG(LS_ERROR) << "Unable to get parent item";
        return;
    }

    std::unique_ptr<proto::address_book::ComputerGroup> computer_group =
        std::make_unique<proto::address_book::ComputerGroup>();
    proto::address_book::ComputerGroupConfig* group_config = computer_group->mutable_config();

    ComputerFactory::setDefaultDesktopManageConfig(
        group_config->mutable_session_config()->mutable_desktop_manage());
    ComputerFactory::setDefaultDesktopViewConfig(
        group_config->mutable_session_config()->mutable_desktop_view());

    proto::address_book::InheritConfig* inherit = group_config->mutable_inherit();
    inherit->set_credentials(true);
    inherit->set_desktop_manage(true);
    inherit->set_desktop_view(true);

    ComputerGroupDialog dialog(this,
                               ComputerGroupDialog::CreateComputerGroup,
                               parentName(parent_item),
                               computer_group.get());
    if (dialog.exec() != QDialog::Accepted)
    {
        LOG(LS_INFO) << "[ACTION] Add computer group rejected by user";
        return;
    }

    LOG(LS_INFO) << "[ACTION] Add computer group accepted by user";

    proto::address_book::ComputerGroup* computer_group_released = computer_group.release();

    ComputerGroupItem* item = parent_item->addChildComputerGroup(computer_group_released);
    ui.tree_group->setCurrentItem(item);
    ui.tree_group->sortItems(0, Qt::AscendingOrder);
    noteEdited();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::addComputer()
{
    LOG(LS_INFO) << "[ACTION] Add computer";
    stopOnlineChecker();

    ComputerGroupItem* parent_item =
        dynamic_cast<ComputerGroupItem*>(ui.tree_group->currentItem());
    if (!parent_item)
        return;

    ComputerDialog dialog(this,
                          ComputerDialog::Mode::CREATE,
                          parentName(parent_item));
    if (dialog.exec() != QDialog::Accepted)
    {
        LOG(LS_INFO) << "[ACTION] Add computer rejected by user";
        return;
    }

    LOG(LS_INFO) << "[ACTION] Add computer accepted by user";

    proto::address_book::Computer* computer =
        new proto::address_book::Computer(dialog.computer());

    parent_item->addChildComputer(computer);
    if (ui.tree_group->currentItem() == parent_item)
    {
        ui.tree_computer->addTopLevelItem(new ComputerItem(computer, parent_item));
    }

    noteEdited();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::copyComputer()
{
    LOG(LS_INFO) << "[ACTION] Copy computer";
    stopOnlineChecker();

    ComputerItem* current_item = dynamic_cast<ComputerItem*>(ui.tree_computer->currentItem());
    if (!current_item)
    {
        LOG(LS_ERROR) << "Unable to get current item";
        return;
    }

    ComputerGroupItem* parent_group_item = current_item->parentComputerGroupItem();
    if (!parent_group_item)
    {
        LOG(LS_ERROR) << "Unable to get parent group item";
        return;
    }

    ComputerDialog dialog(this,
                          ComputerDialog::Mode::COPY,
                          parentName(parent_group_item),
                          *current_item->computer());
    if (dialog.exec() != QDialog::Accepted)
    {
        LOG(LS_INFO) << "[ACTION] Copy computer rejected by user";
        return;
    }

    LOG(LS_INFO) << "[ACTION] Copy computer accepted by user";

    proto::address_book::Computer* computer =
        new proto::address_book::Computer(dialog.computer());

    parent_group_item->addChildComputer(computer);
    if (ui.tree_group->currentItem() == parent_group_item)
    {
        ui.tree_computer->addTopLevelItem(new ComputerItem(computer, parent_group_item));
    }

    noteEdited();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::modifyAddressBook()
{
    LOG(LS_INFO) << "[ACTION] Modify address book";
    stopOnlineChecker();

    ComputerGroupItem* root_item = rootComputerGroupItem();
    if (!root_item)
    {
        LOG(LS_ERROR) << "Invalid root item";
        return;
    }

    AddressBookDialog dialog(this, file_path_, &file_, &data_, &key_);
    if (dialog.exec() != QDialog::Accepted)
    {
        LOG(LS_INFO) << "[ACTION] Address book not modified";
        return;
    }

    LOG(LS_INFO) << "[ACTION] Address book modified";

    root_item->updateItem();
    noteEdited();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::modifyComputerGroup()
{
    LOG(LS_INFO) << "[ACTION] Modify computer group";
    stopOnlineChecker();

    ComputerGroupItem* current_item =
        dynamic_cast<ComputerGroupItem*>(ui.tree_group->currentItem());
    if (!current_item)
    {
        LOG(LS_ERROR) << "Unable to get current item for group";
        return;
    }

    ComputerGroupItem* parent_item = dynamic_cast<ComputerGroupItem*>(current_item->parent());
    if (!parent_item)
    {
        LOG(LS_ERROR) << "Unable to get parent item for group";
        return;
    }

    proto::address_book::ComputerGroup* computer_group = current_item->computerGroup();
    if (!computer_group->has_config())
    {
        proto::address_book::ComputerGroupConfig* group_config = computer_group->mutable_config();

        ComputerFactory::setDefaultDesktopManageConfig(
            group_config->mutable_session_config()->mutable_desktop_manage());
        ComputerFactory::setDefaultDesktopViewConfig(
            group_config->mutable_session_config()->mutable_desktop_view());

        proto::address_book::InheritConfig* inherit = group_config->mutable_inherit();
        inherit->set_credentials(true);
        inherit->set_desktop_manage(true);
        inherit->set_desktop_view(true);
    }

    ComputerGroupDialog dialog(this,
                               ComputerGroupDialog::ModifyComputerGroup,
                               parentName(parent_item),
                               computer_group);
    if (dialog.exec() != QDialog::Accepted)
    {
        LOG(LS_INFO) << "[ACTION] Computer group not modified";
        return;
    }

    LOG(LS_INFO) << "[ACTION] Computer group modified";

    current_item->updateItem();
    noteEdited();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::modifyComputer()
{
    LOG(LS_INFO) << "[ACTION] Modify computer";
    stopOnlineChecker();

    ComputerItem* current_item = dynamic_cast<ComputerItem*>(ui.tree_computer->currentItem());
    if (!current_item)
    {
        LOG(LS_ERROR) << "Unable to get current item";
        return;
    }

    ComputerDialog dialog(this,
                          ComputerDialog::Mode::MODIFY,
                          parentName(current_item->parentComputerGroupItem()),
                          *current_item->computer());
    if (dialog.exec() != QDialog::Accepted)
    {
        LOG(LS_INFO) << "[ACTION] Computer not modified";
        return;
    }

    LOG(LS_INFO) << "[ACTION] Computer modified";

    current_item->computer()->CopyFrom(dialog.computer());
    current_item->updateItem();
    noteEdited();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::removeComputerGroup()
{
    LOG(LS_INFO) << "[ACTION] Remove computer group";
    stopOnlineChecker();

    ComputerGroupItem* current_item =
        dynamic_cast<ComputerGroupItem*>(ui.tree_group->currentItem());
    if (!current_item)
    {
        LOG(LS_ERROR) << "Unable to get current item for group";
        return;
    }

    ComputerGroupItem* parent_item = dynamic_cast<ComputerGroupItem*>(current_item->parent());
    if (!parent_item)
    {
        LOG(LS_ERROR) << "Unable to get parent item for group";
        return;
    }

    QString message =
        tr("Are you sure you want to delete computer group \"%1\" and all child items?")
        .arg(QString::fromStdString(current_item->computerGroup()->name()));

    QMessageBox message_box(QMessageBox::Question,
                            tr("Confirmation"),
                            message,
                            QMessageBox::Yes | QMessageBox::No,
                            this);
    message_box.button(QMessageBox::Yes)->setText(tr("Yes"));
    message_box.button(QMessageBox::No)->setText(tr("No"));

    if (message_box.exec() == QMessageBox::Yes)
    {
        LOG(LS_INFO) << "[ACTION] Computer group removing confirmed by user";
        cleanupComputerGroup(current_item->computerGroup());

        if (parent_item->deleteChildComputerGroup(current_item))
            noteEdited();
    }
    else
    {
        LOG(LS_INFO) << "[ACTION] Computer group removing rejected by user";
    }
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::removeComputer()
{
    LOG(LS_INFO) << "[ACTION] Remove computer";
    stopOnlineChecker();

    ComputerItem* current_item = dynamic_cast<ComputerItem*>(ui.tree_computer->currentItem());
    if (!current_item)
    {
        LOG(LS_ERROR) << "Unable to get current item";
        return;
    }

    QString message = tr("Are you sure you want to delete computer \"%1\"?")
        .arg(QString::fromStdString(current_item->computer()->name()));

    QMessageBox message_box(QMessageBox::Question,
                            tr("Confirmation"),
                            message,
                            QMessageBox::Yes | QMessageBox::No,
                            this);
    message_box.button(QMessageBox::Yes)->setText(tr("Yes"));
    message_box.button(QMessageBox::No)->setText(tr("No"));

    if (message_box.exec() == QMessageBox::Yes)
    {
        LOG(LS_INFO) << "[ACTION] Computer removing confirmed by user";
        ComputerGroupItem* parent_group = current_item->parentComputerGroupItem();

        cleanupComputer(current_item->computer());

        if (parent_group->deleteChildComputer(current_item->computer()))
        {
            delete current_item;
            noteEdited();
        }
    }
    else
    {
        LOG(LS_INFO) << "[ACTION] Computer removing rejected by user";
    }
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::startOnlineChecker()
{
    stopOnlineChecker();

    client::OnlineChecker::ComputerList computers;

    for (int i = 0; i < ui.tree_computer->topLevelItemCount(); ++i)
    {
        ComputerItem* computer_item = static_cast<ComputerItem*>(ui.tree_computer->topLevelItem(i));
        if (!computer_item)
        {
            LOG(LS_ERROR) << "Unable to get computer item: " << i;
            continue;
        }

        proto::address_book::Computer* computer = computer_item->computer();
        if (!computer)
        {
            LOG(LS_ERROR) << "Unable to get computer: " << i;
            continue;
        }

        client::OnlineChecker::Computer computer_to_check;
        computer_to_check.computer_id = computer_item->computerId();
        computer_to_check.address_or_id = base::utf16FromUtf8(computer->address());
        computer_to_check.port = computer->port();

        computers.emplace_back(std::move(computer_to_check));
    }

    if (computers.empty())
    {
        LOG(LS_INFO) << "No computers to check";
        return;
    }

    emit sig_updateStateForComputers(true);

    online_checker_ = std::make_unique<client::OnlineChecker>(qt_base::Application::uiTaskRunner());
    online_checker_->checkComputers(routerConfig(), computers, this);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::stopOnlineChecker()
{
    if (online_checker_)
    {
        LOG(LS_INFO) << "Destory online checker";
        online_checker_.reset();
    }

    for (int i = 0; i < ui.tree_computer->topLevelItemCount(); ++i)
    {
        ComputerItem* computer_item = static_cast<ComputerItem*>(ui.tree_computer->topLevelItem(i));

        computer_item->setIcon(ComputerItem::COLUMN_INDEX_NAME, QIcon(":/img/computer.png"));
        computer_item->setText(ComputerItem::COLUMN_INDEX_STATUS, QString());
    }

    emit sig_updateStateForComputers(false);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onGroupItemClicked(QTreeWidgetItem* item, int /* column */)
{
    LOG(LS_INFO) << "[ACTION] Group item clicked";

    ComputerGroupItem* current_item = dynamic_cast<ComputerGroupItem*>(item);
    if (!current_item)
    {
        LOG(LS_ERROR) << "Unable to get current item";
        return;
    }

    bool is_root = !current_item->parent();
    emit sig_computerGroupActivated(true, is_root);
    updateComputerList(current_item);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onGroupContextMenu(const QPoint& point)
{
    LOG(LS_INFO) << "[ACTION] Group context menu";

    ComputerGroupItem* current_item =
        dynamic_cast<ComputerGroupItem*>(ui.tree_group->itemAt(point));
    if (!current_item)
    {
        LOG(LS_ERROR) << "Unable to get current item";
        return;
    }

    ui.tree_group->setCurrentItem(current_item);
    onGroupItemClicked(current_item, 0);

    bool is_root = !current_item->parent();
    emit sig_computerGroupContextMenu(ui.tree_group->viewport()->mapToGlobal(point), is_root);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onGroupItemCollapsed(QTreeWidgetItem* item)
{
    LOG(LS_INFO) << "[ACTION] Group item collapsed";

    ComputerGroupItem* current_item = dynamic_cast<ComputerGroupItem*>(item);
    if (!current_item)
    {
        LOG(LS_INFO) << "Unable to get current item";
        return;
    }

    current_item->SetExpanded(false);

    // Not sent: which folders are open is a state of this person's window. Still written, so it
    // is the same next time the book is opened.
    setChanged(true);
    autoSave();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onGroupItemExpanded(QTreeWidgetItem* item)
{
    LOG(LS_INFO) << "[ACTION] Group item expanded";

    ComputerGroupItem* current_item = dynamic_cast<ComputerGroupItem*>(item);
    if (!current_item)
    {
        LOG(LS_ERROR) << "Unable to get current item";
        return;
    }

    current_item->SetExpanded(true);

    setChanged(true);
    autoSave();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onGroupItemDropped()
{
    LOG(LS_INFO) << "[ACTION] Group item dropped";

    ComputerGroupItem* current_item =
        dynamic_cast<ComputerGroupItem*>(ui.tree_group->currentItem());
    if (!current_item)
    {
        LOG(LS_ERROR) << "Unable to get current item";
        return;
    }

    ui.tree_group->sortItems(0, Qt::AscendingOrder);
    updateComputerList(current_item);
    noteEdited();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onComputerItemClicked(QTreeWidgetItem* item, int /* column */)
{
    LOG(LS_INFO) << "[ACTION] Computer item clicked";

    ComputerItem* current_item = dynamic_cast<ComputerItem*>(item);
    if (!current_item)
    {
        LOG(LS_ERROR) << "Unable to get current item";
        return;
    }

    emit sig_computerActivated(true);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onComputerContextMenu(const QPoint& point)
{
    LOG(LS_INFO) << "[ACTION] Computer context menu";

    ComputerItem* current_item = dynamic_cast<ComputerItem*>(ui.tree_computer->itemAt(point));
    if (current_item)
    {
        ui.tree_computer->setCurrentItem(current_item);
        onComputerItemClicked(current_item, 0);
    }

    emit sig_computerContextMenu(current_item, ui.tree_computer->viewport()->mapToGlobal(point));
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onComputerItemDoubleClicked(QTreeWidgetItem* item, int /* column */)
{
    LOG(LS_INFO) << "[ACTION] Computer item double clicked";

    ComputerItem* current_item = dynamic_cast<ComputerItem*>(item);
    if (!current_item)
    {
        LOG(LS_ERROR) << "Unable to get current item";
        return;
    }

    emit sig_computerDoubleClicked(current_item->computerToConnect());
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::showEvent(QShowEvent* event)
{
    ComputerGroupItem* current_group =
        dynamic_cast<ComputerGroupItem*>(ui.tree_group->currentItem());
    if (!current_group)
    {
        emit sig_computerGroupActivated(false, false);
    }
    else
    {
        bool is_root = !current_group->parent();
        emit sig_computerGroupActivated(true, is_root);
    }

    ComputerItem* current_computer = dynamic_cast<ComputerItem*>(ui.tree_computer->currentItem());

    if (!current_computer)
        emit sig_computerActivated(false);
    else
        emit sig_computerActivated(true);

    QWidget::showEvent(event);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::keyPressEvent(QKeyEvent* event)
{
    QWidget* focus_widget = QApplication::focusWidget();

    switch (event->key())
    {
        case Qt::Key_Insert:
        {
            LOG(LS_INFO) << "[ACTION] Insert key pressed";
            if (focus_widget == ui.tree_group)
                addComputerGroup();
            else if (focus_widget == ui.tree_computer)
                addComputer();
        }
        break;

        case Qt::Key_F2:
        {
            if (focus_widget == ui.tree_group)
            {
                ComputerGroupItem* current_item =
                    dynamic_cast<ComputerGroupItem*>(ui.tree_group->currentItem());
                if (!current_item)
                {
                    LOG(LS_ERROR) << "Unable to get current item";
                    break;
                }

                LOG(LS_INFO) << "[ACTION] F2 key pressed";

                if (current_item->parent())
                    modifyComputerGroup();
                else
                    modifyAddressBook();
            }
            else if (focus_widget == ui.tree_computer)
            {
                LOG(LS_INFO) << "[ACTION] F2 key pressed";
                modifyComputer();
            }
        }
        break;

        case Qt::Key_Delete:
        {
            LOG(LS_INFO) << "[ACTION] Delete key pressed";
            if (focus_widget == ui.tree_group)
                removeComputerGroup();
            else if (focus_widget == ui.tree_computer)
                removeComputer();
        }
        break;

        case Qt::Key_Return:
        {
            if (focus_widget == ui.tree_computer)
            {
                LOG(LS_INFO) << "[ACTION] Enter key pressed";
                ComputerItem* current_item =
                    dynamic_cast<ComputerItem*>(ui.tree_computer->currentItem());
                if (!current_item)
                    break;

                emit sig_computerDoubleClicked(current_item->computerToConnect());
            }
        }
        break;

        default:
            break;
    }

    QWidget::keyPressEvent(event);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onOnlineCheckerResult(int computer_id, bool online)
{
    ComputerItem* item = nullptr;

    for (int i = 0; i < ui.tree_computer->topLevelItemCount(); ++i)
    {
        item = static_cast<ComputerItem*>(ui.tree_computer->topLevelItem(i));
        if (!item)
            return;

        if (item->computerId() == computer_id)
            break;
    }

    if (!item)
    {
        LOG(LS_ERROR) << "Computer with id " << computer_id << " not found in list";
        return;
    }

    QIcon icon;
    QString status;

    if (online)
    {
        icon = QIcon(":/img/computer-online.png");
        status = tr("Online");
    }
    else
    {
        icon = QIcon(":/img/computer-offline.png");
        status = tr("Offline");
    }

    item->setIcon(ComputerItem::COLUMN_INDEX_NAME, icon);
    item->setText(ComputerItem::COLUMN_INDEX_STATUS, status);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onOnlineCheckerFinished()
{
    LOG(LS_INFO) << "Online checked finished";

    QTimer::singleShot(0, this, [this]()
    {
        if (online_checker_)
        {
            LOG(LS_INFO) << "Destory online checked";
            online_checker_.reset();
        }

        emit sig_updateStateForComputers(false);
    });
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::setChanged(bool value)
{
    is_changed_ = value;
    emit sig_addressBookChanged(value);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::noteEdited()
{
    // One edit made by the person: it goes to the disk and then to the colleagues.
    //
    // This is deliberately not hung off setChanged, which everything that touches the book calls,
    // including what arrives from the router. Sending from there fed itself: what arrived marked
    // the book changed, that sent, the answer marked it changed again, and it went round as fast
    // as the disk allowed.
    setChanged(true);
    autoSave();
    startSyncIfEnabled();
}

//--------------------------------------------------------------------------------------------------
bool AddressBookTab::isSyncEnabled() const
{
    return !data_.sync().book_guid().empty();
}

//--------------------------------------------------------------------------------------------------
QString AddressBookTab::syncBookGuid() const
{
    return QString::fromStdString(data_.sync().book_guid());
}

//--------------------------------------------------------------------------------------------------
bool AddressBookTab::enableSync(const QString& book_guid, const QByteArray& salt,
                                const QByteArray& verifier, const QString& passphrase,
                                bool replace_local)
{
    const std::string key = deriveSyncKey(passphrase.toStdString(), salt.toStdString());
    if (key.empty())
    {
        LOG(LS_ERROR) << "Unable to derive the synchronization key";
        return false;
    }

    // Checked before anything is written. Somebody who mistyped the passphrase would otherwise
    // seal records with a key nobody else has and send them, and nothing would say whose fault it
    // was or when it happened.
    if (!checkKeyVerifier(key, verifier.toStdString()))
    {
        LOG(LS_ERROR) << "The passphrase does not match the shared book";
        return false;
    }

    std::optional<client::RouterConfig> router = routerConfig();
    if (!router.has_value())
    {
        LOG(LS_ERROR) << "The book has no router to synchronize through";
        return false;
    }

    if (replace_local)
    {
        // What is here now is set aside rather than sent: it is already in the copy made of the
        // file, and sending it would put every machine the department shares into the book a
        // second time under a different identity. The root stays - it is this person's own.
        proto::address_book::ComputerGroup* root = data_.mutable_root_group();
        cleanupComputerGroup(root);
        root->clear_computer();
        root->clear_computer_group();
    }

    // Whatever this file remembers about some earlier book means nothing for this one.
    data_.mutable_sync()->clear_entry();

    // Every record needs an identity before it can be sent anywhere.
    ensureEntryGuids(data_.mutable_root_group());

    data_.mutable_sync()->set_book_guid(book_guid.toStdString());
    data_.mutable_sync()->clear_epoch();
    data_.mutable_sync()->set_last_pulled_revision(0);

    // Kept so that opening the book tomorrow does not ask for the shared passphrase again. See
    // SyncState in address_book.proto for why that is not a step backwards.
    data_.mutable_sync()->set_sync_key(key);

    sync_stopped_ = false;

    startSync(key, router.value());

    setChanged(true);
    autoSave();

    // The tree was emptied under the window, and the window points into it.
    if (replace_local)
        reloadAll();

    emit sig_syncStatusChanged();
    return true;
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::resumeSyncIfEnabled()
{
    if (!isSyncEnabled() || book_controller_)
        return;

    if (data_.sync().sync_key().empty())
    {
        // A book that says it is synchronized but carries no key cannot go on by itself. It comes
        // of a file written before the key was kept, and joining the book again settles it.
        LOG(LS_ERROR) << "The book is synchronized but holds no key; it has to be joined again";
        sync_stopped_ = true;
        emit sig_syncStatusChanged();
        return;
    }

    std::optional<client::RouterConfig> router = routerConfig();
    if (!router.has_value())
    {
        LOG(LS_ERROR) << "The book has no router to synchronize through";
        sync_stopped_ = true;
        emit sig_syncStatusChanged();
        return;
    }

    startSync(data_.sync().sync_key(), router.value());

    emit sig_syncStatusChanged();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::startSync(const std::string& key, const client::RouterConfig& router)
{
    book_sync_ = std::make_unique<BookSync>(key, this, this);
    book_controller_ = std::make_unique<BookController>(
        router, qt_base::Application::uiTaskRunner());
    book_controller_->start(this);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::disableSync()
{
    book_controller_.reset();
    book_sync_.reset();

    sync_connected_ = false;
    sync_stopped_ = false;

    // What was fetched stays. The book becomes an ordinary local file again, which is the way back
    // if any of this turns out to be a bad idea. The key goes with it - there is nothing left for
    // it to open.
    data_.mutable_sync()->Clear();

    setChanged(true);

    // Written at once, and not through autoSave: that only saves books that are synchronized, which
    // this one stopped being a line ago. Left unsaved, the file would still say it is joined, and
    // the next start would quietly join it again.
    if (!file_path_.isEmpty())
        saveToFile(file_path_);

    emit sig_syncStatusChanged();
}

//--------------------------------------------------------------------------------------------------
AddressBookTab::SyncStatus AddressBookTab::syncStatus() const
{
    SyncStatus status;
    status.enabled = isSyncEnabled();
    status.connected = sync_connected_;
    status.stopped = sync_stopped_;

    if (book_sync_)
        status.conflicts = static_cast<int>(book_sync_->conflicts().size());

    for (int i = 0; i < data_.sync().entry_size(); ++i)
    {
        const proto::address_book::SyncEntryState& state = data_.sync().entry(i);
        if (state.dirty() || state.deleted())
            ++status.pending;
    }

    return status;
}

//--------------------------------------------------------------------------------------------------
std::vector<std::string> AddressBookTab::syncConflicts() const
{
    if (!book_sync_)
        return std::vector<std::string>();

    return book_sync_->conflicts();
}

//--------------------------------------------------------------------------------------------------
QString AddressBookTab::computerNameByGuid(const QString& guid) const
{
    const std::string needle = guid.toStdString();

    for (const FlatEntry& entry : flattenBook(data_.root_group()))
    {
        if (entry.guid != needle)
            continue;

        proto::address_book::Computer computer;
        if (computer.ParseFromString(entry.payload) && !computer.name().empty())
            return QString::fromStdString(computer.name());

        proto::address_book::ComputerGroup group;
        if (group.ParseFromString(entry.payload) && !group.name().empty())
            return QString::fromStdString(group.name());

        break;
    }

    return guid;
}

//--------------------------------------------------------------------------------------------------
bool AddressBookTab::resolveSyncConflict(const QString& guid, bool keep_local)
{
    if (!book_sync_)
        return false;

    // The book is written and redrawn by onBookUpdated, which the resolution triggers on its way
    // through, so there is nothing to do here but report whether it took.
    return book_sync_->resolveConflict(guid.toStdString(), keep_local, &data_);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::startSyncIfEnabled()
{
    if (!book_sync_ || !sync_connected_ || sync_stopped_)
        return;

    book_sync_->start(&data_);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::autoSave()
{
    // A synchronized book is not a document somebody remembers to save. An edit that reached the
    // colleagues but not the disk would come back as a surprise after the next restart, and the
    // window between the two is exactly what the atomic write was put in for.
    if (!isSyncEnabled() || file_path_.isEmpty() || !is_changed_)
        return;

    saveToFile(file_path_);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onBookConnected()
{
    LOG(LS_INFO) << "Address book synchronization is connected";

    sync_connected_ = true;
    emit sig_syncStatusChanged();

    startSyncIfEnabled();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onBookDisconnected()
{
    sync_connected_ = false;
    emit sig_syncStatusChanged();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onBookAuthFailed()
{
    sync_connected_ = false;
    sync_stopped_ = true;
    emit sig_syncStatusChanged();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onBookList(const proto::BookList& /* message */)
{
    // Asked for only while joining a book, which the wizard drives.
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onBookPull(const proto::BookPull& message)
{
    if (!book_sync_)
        return;

    if (holdWhileDialogIsOpen([this, message]() { onBookPull(message); }))
        return;

    book_sync_->onPull(message, &data_);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onBookPushResult(const proto::BookPushResult& message)
{
    if (!book_sync_)
        return;

    if (holdWhileDialogIsOpen([this, message]() { onBookPushResult(message); }))
        return;

    book_sync_->onPushResult(message, &data_);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onBookChanged(const proto::BookChanged& message)
{
    if (!book_sync_)
        return;

    if (holdWhileDialogIsOpen([this, message]() { onBookChanged(message); }))
        return;

    book_sync_->onBookChanged(message, &data_);
}

//--------------------------------------------------------------------------------------------------
bool AddressBookTab::holdWhileDialogIsOpen(std::function<void()> again)
{
    if (!QApplication::activeModalWidget())
    {
        if (holding_)
        {
            LOG(LS_INFO) << "The dialog is closed; taking in what the router sent";
            holding_ = false;
        }
        return false;
    }

    if (!holding_)
    {
        // Said once rather than every time it looks, which is several times a second for as long
        // as the dialog stays open.
        LOG(LS_INFO) << "A dialog is open; holding what the router sent until it closes";
        holding_ = true;
    }

    // Somebody has a dialog open - the properties of a computer, most likely. Taking a colleague's
    // change in now would replace the tree under it, and the tree is what the window points into:
    // the dialog was handed a record that would no longer exist, and pressing OK would write
    // through a pointer to freed memory.
    //
    // So it waits. What arrived is held and tried again shortly; the exchange is idle meanwhile,
    // which is the right thing for it to be while somebody is in the middle of typing.
    QTimer::singleShot(kDialogRetryMs, this, [again]() { again(); });
    return true;
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::sendPull(const proto::BookPullRequest& request)
{
    if (book_controller_)
        book_controller_->requestPull(request);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::sendPush(const proto::BookPushRequest& request)
{
    if (book_controller_)
        book_controller_->requestPush(request);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onBookUpdated()
{
    // What arrived has to reach the disk before it reaches the window: a crash in between would
    // otherwise leave the person looking at records the file does not have.
    //
    // It is not sent back. BookSync carries its own exchange to the end, and pushing from here
    // would answer its own answer for as long as the console stayed open.
    setChanged(true);
    autoSave();

    reloadAll();

    emit sig_syncStatusChanged();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onConflicts(const std::vector<std::string>& /* guids */)
{
    // Nothing is interrupted. The status bar says how many are waiting and the person goes to them
    // when it suits; a window in the middle of something else gets closed without being read.
    emit sig_syncStatusChanged();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onSyncStopped(SyncEngine::PullOutcome::Status reason)
{
    LOG(LS_ERROR) << "Address book synchronization stopped, reason: " << static_cast<int>(reason);

    sync_stopped_ = true;
    emit sig_syncStatusChanged();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onInSync(int64_t /* revision */)
{
    // Nothing is waiting any more, and the revision that says so is worth keeping: the next start
    // then asks for what happened since, rather than for the whole book.
    setChanged(true);
    autoSave();

    emit sig_syncStatusChanged();
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::retranslateUi()
{
    ui.retranslateUi(this);

    ComputerGroupItem* root_item = rootComputerGroupItem();
    if (root_item)
        root_item->updateItem();

    QTreeWidgetItem* current = ui.tree_group->currentItem();
    if (current)
        onGroupItemClicked(current, 0);
}

//--------------------------------------------------------------------------------------------------
QByteArray AddressBookTab::saveState()
{
    QByteArray buffer;

    {
        QDataStream stream(&buffer, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_5_12);

        stream << ui.tree_computer->header()->saveState();
        stream << ui.splitter->saveState();
    }

    return buffer;
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::restoreState(const QByteArray& state)
{
    QDataStream stream(state);
    stream.setVersion(QDataStream::Qt_5_12);

    QByteArray columns_state;
    QByteArray splitter_state;

    stream >> columns_state;
    stream >> splitter_state;

    if (!columns_state.isEmpty())
    {
        ui.tree_computer->header()->restoreState(columns_state);
    }

    if (!splitter_state.isEmpty())
    {
        ui.splitter->restoreState(splitter_state);
    }
    else
    {
        QList<int> sizes;
        sizes.push_back(200);
        sizes.push_back(width() - 200);
        ui.splitter->setSizes(sizes);
    }
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::updateComputerList(ComputerGroupItem* computer_group)
{
    if (!search_text_.isEmpty())
    {
        // A search is on, so the list is not about the folder that was just selected.
        showSearchResults(search_text_);
        return;
    }

    if (online_checker_)
    {
        LOG(LS_INFO) << "Destroy online checker";
        online_checker_.reset();
    }

    for (int i = ui.tree_computer->topLevelItemCount() - 1; i >= 0; --i)
        std::unique_ptr<QTreeWidgetItem> item_deleter(ui.tree_computer->takeTopLevelItem(i));

    ui.tree_computer->addTopLevelItems(computer_group->ComputerList());
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::showSearchResults(const QString& text)
{
    if (online_checker_)
    {
        LOG(LS_INFO) << "Destroy online checker";
        online_checker_.reset();
    }

    for (int i = ui.tree_computer->topLevelItemCount() - 1; i >= 0; --i)
        std::unique_ptr<QTreeWidgetItem> item_deleter(ui.tree_computer->takeTopLevelItem(i));

    ComputerGroupItem* root_item = rootComputerGroupItem();
    if (!root_item)
        return;

    QList<QTreeWidgetItem*> found;

    // |path| is what is shown in the folder column, and |matched_here| says that a folder along
    // the way already matched - everything under it then counts, which is what makes searching by
    // folder name mean "show me what is in there".
    std::function<void(ComputerGroupItem*, const QString&, bool)> walk =
        [&](ComputerGroupItem* group_item, const QString& path, bool matched_here)
    {
        for (QTreeWidgetItem* item : group_item->ComputerList())
        {
            ComputerItem* computer_item = static_cast<ComputerItem*>(item);
            const proto::address_book::Computer* computer = computer_item->computer();

            const bool matches = matched_here ||
                QString::fromStdString(computer->name()).contains(text, Qt::CaseInsensitive) ||
                QString::fromStdString(computer->address()).contains(text, Qt::CaseInsensitive);

            if (!matches)
            {
                delete computer_item;
                continue;
            }

            computer_item->setText(ComputerItem::COLUMN_INDEX_FOLDER,
                                   path.isEmpty() ? parentName(group_item) : path);
            found.push_back(computer_item);
        }

        for (int i = 0; i < group_item->childCount(); ++i)
        {
            ComputerGroupItem* child = dynamic_cast<ComputerGroupItem*>(group_item->child(i));
            if (!child)
                continue;

            const QString name = QString::fromStdString(child->computerGroup()->name());
            const QString child_path = path.isEmpty() ? name : path + QLatin1Char('/') + name;

            walk(child, child_path, matched_here || name.contains(text, Qt::CaseInsensitive));
        }
    };

    walk(root_item, QString(), false);

    ui.tree_computer->addTopLevelItems(found);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::refreshComputerList()
{
    if (!search_text_.isEmpty())
    {
        showSearchResults(search_text_);
        return;
    }

    ComputerGroupItem* current_item = dynamic_cast<ComputerGroupItem*>(ui.tree_group->currentItem());
    if (current_item)
        updateComputerList(current_item);
}

//--------------------------------------------------------------------------------------------------
void AddressBookTab::onSearchTextChanged(const QString& text)
{
    search_text_ = text.trimmed();

    const bool searching = !search_text_.isEmpty();

    ui.tree_computer->setColumnHidden(ComputerItem::COLUMN_INDEX_FOLDER, !searching);

    // And again here, for a book whose saved layout was written before this column existed.
    if (searching && ui.tree_computer->columnWidth(ComputerItem::COLUMN_INDEX_FOLDER) <= 0)
        ui.tree_computer->setColumnWidth(ComputerItem::COLUMN_INDEX_FOLDER, kFolderColumnWidth);

    refreshComputerList();
}

//--------------------------------------------------------------------------------------------------
bool AddressBookTab::saveToFile(const QString& file_path)
{
    LOG(LS_INFO) << "Save address book to file: '" << file_path.toStdString() << "'";

    // Records made since the last save - by the dialogs, by an import - have no guid yet. This is
    // the one place every one of them passes through, so they are given an identity here rather
    // than in every place that can add a record.
    //
    // It has to happen before the book is serialized, or the guids would be assigned to the copy
    // in memory and left out of the file: every record would come back without one after a
    // restart, be given a new one, and look to the router like a record nobody had seen before.
    ensureEntryGuids(data_.mutable_root_group());

    std::string serialized_data = data_.SerializeAsString();
    std::unique_ptr<base::DataCryptor> cryptor;

    switch (file_.encryption_type())
    {
        case proto::address_book::ENCRYPTION_TYPE_NONE:
            cryptor = std::make_unique<base::DataCryptorFake>();
            break;

        case proto::address_book::ENCRYPTION_TYPE_CHACHA20_POLY1305:
            cryptor = std::make_unique<base::DataCryptorChaCha20Poly1305>(key_);
            break;

        default:
            LOG(LS_FATAL) << "Unknown encryption type: " << file_.encryption_type();
            return false;
    }

    std::string encrypted_data;
    CHECK(cryptor->encrypt(serialized_data, &encrypted_data));
    base::memZero(&serialized_data);

    file_.set_data(std::move(encrypted_data));

    QString path = file_path;
    if (path.isEmpty())
    {
        LOG(LS_INFO) << "File path is empty. Show dialog to save file";
        Settings settings;

        path = QFileDialog::getSaveFileName(this,
                                            tr("Save Address Book"),
                                            settings.lastDirectory(),
                                            tr("Aspia Address Book (*.aab)"));
        if (path.isEmpty())
        {
            LOG(LS_INFO) << "File path not selected";
            return false;
        }

        LOG(LS_INFO) << "Selected file path: " << path.toStdString();
        settings.setLastDirectory(QFileInfo(path).absolutePath());
    }

    base::ByteArray buffer = base::serialize(file_);

    // The file being written is the only copy of the address book, so it is never opened for
    // writing directly: the content goes to a temporary next to it and is renamed over it. An
    // interrupted save then leaves the previous book whole instead of a truncated file nothing
    // can decrypt.
    const bool written = base::writeFileAtomically(
        std::filesystem::path(path.toStdWString()), buffer);

    base::memZero(buffer.data(), buffer.size());

    if (!written)
    {
        LOG(LS_ERROR) << "Unable to write address book file";
        showSaveError(this, tr("Unable to write address book file."));
        return false;
    }

    file_path_ = path;

    LOG(LS_INFO) << "Address book saved";
    setChanged(false);
    return true;
}

//--------------------------------------------------------------------------------------------------
ComputerGroupItem* AddressBookTab::rootComputerGroupItem()
{
    ComputerGroupItem* root_item = nullptr;

    for (int i = 0; i < ui.tree_group->topLevelItemCount(); ++i)
    {
        root_item = dynamic_cast<ComputerGroupItem*>(ui.tree_group->topLevelItem(i));
        if (root_item)
            break;
    }

    return root_item;
}

//--------------------------------------------------------------------------------------------------
// static
QString AddressBookTab::parentName(ComputerGroupItem* item)
{
    if (!item->parent())
        return tr("Root Group");

    return QString::fromStdString(item->computerGroup()->name());
}

//--------------------------------------------------------------------------------------------------
// static
void AddressBookTab::showOpenError(QWidget* parent, const QString& message)
{
    QMessageBox dialog(parent);

    dialog.setIcon(QMessageBox::Warning);
    dialog.setWindowTitle(tr("Warning"));
    dialog.setInformativeText(message);
    dialog.setText(tr("Could not open address book"));
    dialog.setStandardButtons(QMessageBox::Ok);

    dialog.exec();
}

//--------------------------------------------------------------------------------------------------
// static
void AddressBookTab::showSaveError(QWidget* parent, const QString& message)
{
    QMessageBox dialog(parent);

    dialog.setIcon(QMessageBox::Warning);
    dialog.setWindowTitle(tr("Warning"));
    dialog.setInformativeText(message);
    dialog.setText(tr("Failed to save address book"));
    dialog.setStandardButtons(QMessageBox::Ok);

    dialog.exec();
}

} // namespace console
