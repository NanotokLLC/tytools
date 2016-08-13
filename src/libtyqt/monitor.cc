/*
 * ty, a collection of GUI and command-line tools to manage Teensy devices
 *
 * Distributed under the MIT license (see LICENSE.txt or http://opensource.org/licenses/MIT)
 * Copyright (c) 2015 Niels Martignène <niels.martignene@gmail.com>
 */

#include <QBrush>
#include <QIcon>

#include "tyqt/board.hpp"
#include "tyqt/database.hpp"
#include "tyqt/descriptor_notifier.hpp"
#include "tyqt/monitor.hpp"
#include "hs/platform.h"
#include "ty/task.h"

using namespace std;

Monitor::Monitor(QObject *parent)
    : QAbstractListModel(parent)
{
    int r = ty_pool_new(&pool_);
    if (r < 0)
        throw bad_alloc();

    loadSettings();
}

Monitor::~Monitor()
{
    stop();

    ty_pool_free(pool_);
    ty_monitor_free(monitor_);
}

void Monitor::loadSettings()
{
    auto max_tasks = db_.get("maxTasks").toUInt();
    if (!max_tasks) {
#ifdef _WIN32
        if (hs_win32_version() >= HS_WIN32_VERSION_10) {
            /* Windows 10 is much faster to load drivers and make the device available, we
               can probably afford that. */
            max_tasks = 2;
        } else {
            max_tasks = 1;
        }
#else
        max_tasks = 4;
#endif
    }
    ty_pool_set_max_threads(pool_, max_tasks);
    default_serial_ = db_.get("serialByDefault", true).toBool();

    emit settingsChanged();

    for (auto &board: boards_)
        board->loadSettings(this);
}

void Monitor::setMaxTasks(unsigned int max_tasks)
{
    ty_pool_set_max_threads(pool_, max_tasks);

    db_.put("maxTasks", max_tasks);
    emit settingsChanged();
}

unsigned int Monitor::maxTasks() const
{
    return ty_pool_get_max_threads(pool_);
}

void Monitor::setSerialByDefault(bool default_serial)
{
    default_serial_ = default_serial;

    for (auto &board: boards_) {
        auto db = board->database();

        if (!db.get("enableSerial").isValid()) {
            board->setEnableSerial(default_serial);
            db.remove("enableSerial");
        }
    }

    db_.put("serialByDefault", default_serial);
    emit settingsChanged();
}

bool Monitor::start()
{
    if (started_)
        return true;

    int r;

    if (!monitor_) {
        ty_monitor *monitor;

        r = ty_monitor_new(TY_MONITOR_PARALLEL_WAIT, &monitor);
        if (r < 0)
            return false;
        unique_ptr<ty_monitor, decltype(&ty_monitor_free)> monitor_ptr(monitor, ty_monitor_free);

        r = ty_monitor_register_callback(monitor, handleEvent, this);
        if (r < 0)
            return false;

        ty_descriptor_set set = {};
        ty_monitor_get_descriptors(monitor, &set, 1);
        monitor_notifier_.setDescriptorSet(&set);
        connect(&monitor_notifier_, &DescriptorNotifier::activated, this, &Monitor::refresh);

        monitor_ = monitor_ptr.release();
    }

    serial_thread_.start();

    r = ty_monitor_start(monitor_);
    if (r < 0)
        return false;
    monitor_notifier_.setEnabled(true);

    started_ = true;
    return true;
}

void Monitor::stop()
{
    if (!started_)
        return;

    serial_thread_.quit();
    serial_thread_.wait();

    if (!boards_.empty()) {
        beginRemoveRows(QModelIndex(), 0, boards_.size());
        boards_.clear();
        endRemoveRows();
    }

    monitor_notifier_.setEnabled(false);
    ty_monitor_stop(monitor_);

    started_ = false;
}

vector<shared_ptr<Board>> Monitor::boards()
{
    return boards_;
}

shared_ptr<Board> Monitor::board(unsigned int i)
{
    if (i >= boards_.size())
        return nullptr;

    return boards_[i];
}

unsigned int Monitor::boardCount() const
{
    return boards_.size();
}

shared_ptr<Board> Monitor::boardFromModel(const QAbstractItemModel *model,
                                          const QModelIndex &index)
{
    auto board = model->data(index, Monitor::ROLE_BOARD).value<Board *>();
    return board ? board->shared_from_this() : nullptr;
}

shared_ptr<Board> Monitor::find(function<bool(Board &board)> filter)
{
    auto board = find_if(boards_.begin(), boards_.end(), [&](shared_ptr<Board> &ptr) { return filter(*ptr); });

    if (board == boards_.end())
        return nullptr;

    return *board;
}

int Monitor::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return boards_.size();
}

int Monitor::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return COLUMN_COUNT;
}

QVariant Monitor::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Vertical || role != Qt::DisplayRole)
        return QVariant();

    switch (section) {
    case COLUMN_BOARD:
        return tr("Board");
    case COLUMN_STATUS:
        return tr("Status");
    case COLUMN_IDENTITY:
        return tr("Identity");
    case COLUMN_LOCATION:
        return tr("Location");
    case COLUMN_SERIAL_NUMBER:
        return tr("Serial Number");
    case COLUMN_DESCRIPTION:
        return tr("Description");
    }

    return QVariant();
}

QVariant Monitor::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(boards_.size()))
        return QVariant();

    auto board = boards_[index.row()];
    if (role == ROLE_BOARD)
        return QVariant::fromValue(board.get());

    if (index.column() == 0) {
        switch (role) {
            case Qt::ToolTipRole:
                return tr("%1\n+ Location: %2\n+ Serial Number: %3\n+ Status: %4\n+ Capabilities: %5")
                       .arg(board->modelName())
                       .arg(board->location())
                       .arg(board->serialNumber())
                       .arg(board->statusText())
                       .arg(Board::makeCapabilityString(board->capabilities(), tr("(none)")));
            case Qt::DecorationRole:
                return board->statusIcon();
            case Qt::EditRole:
                return board->tag();
            case Qt::SizeHintRole:
                return QSize(0, 24);
        }
    }

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case COLUMN_BOARD:
            return board->tag();
        case COLUMN_STATUS:
            return board->statusText();
        case COLUMN_IDENTITY:
            return board->id();
        case COLUMN_LOCATION:
            return board->location();
        case COLUMN_SERIAL_NUMBER:
            return static_cast<quint64>(board->serialNumber());
        case COLUMN_DESCRIPTION:
            return board->description();
        }
    }

    return QVariant();
}

Qt::ItemFlags Monitor::flags(const QModelIndex &index) const
{
    Q_UNUSED(index);
    return Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsEnabled;
}

bool Monitor::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !index.isValid() || index.row() >= static_cast<int>(boards_.size()))
        return false;

    auto board = boards_[index.row()];
    board->setTag(value.toString());

    return true;
}

void Monitor::refresh(ty_descriptor desc)
{
    Q_UNUSED(desc);
    ty_monitor_refresh(monitor_);
}

int Monitor::handleEvent(ty_board *board, ty_monitor_event event, void *udata)
{
    auto self = static_cast<Monitor *>(udata);

    switch (event) {
    case TY_MONITOR_EVENT_ADDED:
        self->handleAddedEvent(board);
        break;

    case TY_MONITOR_EVENT_CHANGED:
    case TY_MONITOR_EVENT_DISAPPEARED:
    case TY_MONITOR_EVENT_DROPPED:
        self->handleChangedEvent(board);
        break;
    }

    return 0;
}

Monitor::iterator Monitor::findBoardIterator(ty_board *board)
{
    return find_if(boards_.begin(), boards_.end(),
                   [=](std::shared_ptr<Board> &ptr) { return ptr->board() == board; });
}

void Monitor::handleAddedEvent(ty_board *board)
{
    // Work around the private constructor for make_shared()
    struct BoardSharedEnabler : public Board {
        BoardSharedEnabler(ty_board *board)
            : Board(board) {}
    };
    auto ptr = make_shared<BoardSharedEnabler>(board);

    if (ptr->hasCapability(TY_BOARD_CAPABILITY_UNIQUE)) {
        ptr->setDatabase(db_.subDatabase(ptr->id()));
        ptr->setCache(cache_.subDatabase(ptr->id()));
    }
    ptr->loadSettings(this);

    ptr->setThreadPool(pool_);
    ptr->serial_notifier_.moveToThread(&serial_thread_);

    connect(ptr.get(), &Board::infoChanged, this, [=]() {
        refreshBoardItem(findBoardIterator(board));
    });
    connect(ptr.get(), &Board::interfacesChanged, this, [=]() {
        refreshBoardItem(findBoardIterator(board));
    });
    connect(ptr.get(), &Board::statusChanged, this, [=]() {
        refreshBoardItem(findBoardIterator(board));
    });
    connect(ptr.get(), &Board::progressChanged, this, [=]() {
        refreshBoardItem(findBoardIterator(board));
    });
    connect(ptr.get(), &Board::dropped, this, [=]() {
        removeBoardItem(findBoardIterator(board));
    });

    beginInsertRows(QModelIndex(), boards_.size(), boards_.size());
    boards_.push_back(ptr);
    endInsertRows();

    emit boardAdded(ptr.get());
}

void Monitor::handleChangedEvent(ty_board *board)
{
    auto it = findBoardIterator(board);
    if (it == boards_.end())
        return;
    auto ptr = *it;

    ptr->refreshBoard();
}

void Monitor::refreshBoardItem(iterator it)
{
    auto index = createIndex(it - boards_.begin(), 0);
    dataChanged(index, index);
}

void Monitor::removeBoardItem(iterator it)
{
    beginRemoveRows(QModelIndex(), it - boards_.begin(), it - boards_.begin());
    boards_.erase(it);
    endRemoveRows();
}
