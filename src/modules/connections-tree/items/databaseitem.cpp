#include "databaseitem.h"

#include <QDebug>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <algorithm>
#include <functional>
#include <typeinfo>

#include "connections-tree/model.h"
#include "connections-tree/utils.h"
#include "keyitem.h"
#include "namespaceitem.h"
#include "serveritem.h"

using namespace ConnectionsTree;

DatabaseItem::DatabaseItem(unsigned int index, int keysCount,
                           QSharedPointer<Operations> operations,
                           QWeakPointer<TreeItem> parent, Model& model)
    : AbstractNamespaceItem(model, parent, operations, index),
      m_keysCount(keysCount) {}

DatabaseItem::~DatabaseItem() {
  if (m_operations) m_operations->notifyDbWasUnloaded(m_dbIndex);
}

QByteArray DatabaseItem::getName() const { return QByteArray(); }

QByteArray DatabaseItem::getFullPath() const { return QByteArray(); }

QString DatabaseItem::getDisplayName() const {
  QString filter = m_filter.pattern() == "*"
                       ? ""
                       : QString("[filter: %1]").arg(m_filter.pattern());

  if (m_operations->mode() == "cluster") {
    return QString("db%1 %2").arg(m_dbIndex).arg(filter);
  } else {
    return QString("db%1 %2 (%3)").arg(m_dbIndex).arg(filter).arg(m_keysCount);
  }
}

bool DatabaseItem::isEnabled() const { return true; }

void DatabaseItem::notifyModel() {
  unlock();
  AbstractNamespaceItem::notifyModel();
}

void DatabaseItem::loadKeys(std::function<void()> callback) {
  lock();

  QString filter = (m_filter.isEmpty()) ? "" : m_filter.pattern();

  auto self = getSelf().toStrongRef();

  if (!self) {
    unlock();
    return;
  }

  std::function<void(RedisClient::DatabaseList)> dbLoadCallback =
      [this](QMap<int, int> dbMapping) {
        if (dbMapping.contains(m_dbIndex)) {
          m_keysCount = dbMapping[m_dbIndex];
          emit m_model.itemChanged(getSelf());
        }
      };

  try {
    m_operations->getDatabases(dbLoadCallback);
  } catch (const ConnectionsTree::Operations::Exception& e) {
    unlock();
    emit m_model.error(
        QCoreApplication::translate("RDM", "Cannot load databases:\n\n") +
        QString(e.what()));
  }

  m_operations->loadNamespaceItems(
      qSharedPointerDynamicCast<AbstractNamespaceItem>(self), filter,
      [this, callback](const QString& err) {
        unlock();
        if (!err.isEmpty()) return showLoadingError(err);

        if (callback) {
          callback();
        }
      },
      m_model.m_expanded);
}

QVariantMap DatabaseItem::metadata() const {
  QVariantMap metadata = TreeItem::metadata();
  metadata["filter"] = m_filter.pattern();
  metadata["live_update"] = isLiveUpdateEnabled();
  return metadata;
}

void DatabaseItem::setMetadata(const QString& key, QVariant value) {
  bool isResetValue = (value.isNull() || !value.canConvert<QString>() ||
                       value.toString().isEmpty());

  if (key == "filter") {
    if (!m_filter.isEmpty() && isResetValue)
      return resetFilter();
    else if (isResetValue)
      return;

    QRegExp pattern(value.toString(), Qt::CaseSensitive,
                    QRegExp::PatternSyntax::WildcardUnix);
    return filterKeys(pattern);
  } else if (key == "live_update") {
    if (liveUpdateTimer()->isActive() && isResetValue) {
      qDebug() << "Stop live update";
      liveUpdateTimer()->stop();
    } else {
      qDebug() << "Start live update";
      liveUpdateTimer()->start();
    }

    emit m_model.itemChanged(getSelf());
  }
}

QFuture<qlonglong> DatabaseItem::getMemoryUsage(
    QSharedPointer<AsyncFuture::Combinator> combinator) {
  if (m_childItems.size() == 0) {
    auto d = QSharedPointer<AsyncFuture::Deferred<qlonglong>>(
        new AsyncFuture::Deferred<qlonglong>());
    loadKeys([this, d, combinator]() {
      lock();
      d->complete(AbstractNamespaceItem::getMemoryUsage(combinator));
    });
    return d->future();
  } else {
    return AbstractNamespaceItem::getMemoryUsage(combinator);
  }
}

void DatabaseItem::unload(bool notify) {
  if (m_childItems.size() == 0) return;

  lock();
  clear();

  m_keysCount = 0;

  if (notify) m_operations->notifyDbWasUnloaded(m_dbIndex);

  unlock();
}

void DatabaseItem::reload(std::function<void()> callback) {
  unload(false);
  loadKeys(callback);
}

void DatabaseItem::performLiveUpdate() {
  qDebug() << "Live update loading keys...";

  if (isLocked()) {
    qDebug()
        << "Another loading operation is in progress. Skip this live update...";
    liveUpdateTimer()->start();
    return;
  }

  reload([this]() {
    QSettings settings;
    if (m_childItems.size() >=
        settings.value("app/liveUpdateKeysLimit", 1000).toInt()) {
      liveUpdateTimer()->stop();
      emit m_model.itemChanged(getSelf());
      QMessageBox::warning(
          nullptr,
          QCoreApplication::translate("RDM", "Live update was disabled"),
          QCoreApplication::translate(
              "RDM",
              "Live update was disabled due to exceeded keys limit. "
              "Please specify filter more carrfully or change limit in "
              "settings."));
    } else {
      liveUpdateTimer()->start();
      emit m_model.itemChanged(getSelf());
    }
  });
}

void DatabaseItem::filterKeys(const QRegExp& filter) {
  m_filter = filter;
  emit m_model.itemChanged(getSelf());
  reload();
}

void DatabaseItem::resetFilter() {
  m_filter = QRegExp(m_operations->defaultFilter());
  emit m_model.itemChanged(getSelf());
  reload();
}

QHash<QString, std::function<void()>> DatabaseItem::eventHandlers() {
  auto events = AbstractNamespaceItem::eventHandlers();

  events.insert("click", [this]() {
    if (m_childItems.size() != 0) return;

    loadKeys();
  });

  events.insert("add_key", [this]() {
    m_operations->openNewKeyDialog(m_dbIndex, [this]() {
      confirmAction(
          nullptr,
          QCoreApplication::translate(
              "RDM",
              "Key was added. Do you want to reload keys in "
              "selected database?"),
          [this]() {
            reload();
            m_keysCount++;
          },
          QCoreApplication::translate("RDM", "Key was added"));
    });
  });

  events.insert("reload", [this]() {
    if (isLocked()) {
      QMessageBox::warning(
          nullptr,
          QCoreApplication::translate(
              "RDM", "Another operation is currently in progress"),
          QCoreApplication::translate(
              "RDM", "Please wait until another operation will be finised."));
      return;
    }

    reload();
  });

  events.insert("flush", [this]() {
    confirmAction(
        nullptr,
        QCoreApplication::translate(
            "RDM", "Do you really want to remove all keys from this database?"),
        [this]() {
          m_operations->flushDb(m_dbIndex,
                                [this](const QString&) { unload(); });
        });
  });

  events.insert("console",
                [this]() { m_operations->openConsoleTab(m_dbIndex); });

  return events;
}

QSharedPointer<QTimer> DatabaseItem::liveUpdateTimer() {
  if (!m_liveUpdateTimer) {
    QSettings settings;
    m_liveUpdateTimer = QSharedPointer<QTimer>(new QTimer());
    m_liveUpdateTimer->setInterval(
        settings.value("app/liveUpdateInterval", 10).toInt() * 1000);

    qDebug() << "Live update timer"
             << settings.value("app/liveUpdateInterval", 10).toInt() * 1000;

    m_liveUpdateTimer->setSingleShot(true);

    QObject::connect(m_liveUpdateTimer.data(), &QTimer::timeout,
                     [this]() { performLiveUpdate(); });
  }

  return m_liveUpdateTimer;
}

bool DatabaseItem::isLiveUpdateEnabled() const {
  return m_liveUpdateTimer && m_liveUpdateTimer->isActive();
}
