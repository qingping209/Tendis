// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this tendis open source
// project for additional information.

#include "tendisplus/server/index_manager.h"

#include <chrono>  // NOLINT
#include <memory>
#include <vector>
#include <utility>
#include <string>

#include "glog/logging.h"

#include "tendisplus/commands/command.h"
#include "tendisplus/utils/invariant.h"
#include "tendisplus/utils/sync_point.h"
#include "tendisplus/utils/portable.h"
#include "tendisplus/utils/string.h"
#include "tendisplus/utils/scopeguard.h"


namespace tendisplus {

IndexManager::IndexManager(std::shared_ptr<ServerEntry> svr,
                           std::shared_ptr<ServerParams> cfg)
  : _isRunning(false),
    _svr(svr),
    _scannerMatrix(std::make_shared<PoolMatrix>()),
    _deleterMatrix(std::make_shared<PoolMatrix>()),
    _totalDequeue(0),
    _totalEnqueue(0),
    _scanBatch(cfg->scanCntIndexMgr),
    _scanPoolSize(cfg->scanJobCntIndexMgr),
    _delBatch(cfg->delCntIndexMgr),
    _delPoolSize(cfg->delJobCntIndexMgr),
    _pauseTime(cfg->pauseTimeIndexMgr) {
  for (size_t storeId = 0; storeId < svr->getKVStoreCount(); ++storeId) {
    _scanPoints[storeId] = std::move(std::string());
    _scanJobStatus[storeId] = {false};
    _delJobStatus[storeId] = {false};
    _disableStatus[storeId] = {false};
    _scanJobCnt[storeId] = {0u};
    _delJobCnt[storeId] = {0u};
  }
}

Status IndexManager::startup() {
  Status s;

  _indexScanner = std::make_unique<WorkerPool>("tx-idx-scan", _scannerMatrix);
  s = _indexScanner->startup(_scanPoolSize);
  if (!s.ok()) {
    return s;
  }

  _keyDeleter = std::make_unique<WorkerPool>("tx-idx-del", _deleterMatrix);
  s = _keyDeleter->startup(_delPoolSize);
  if (!s.ok()) {
    return s;
  }

  _isRunning.store(true, std::memory_order_relaxed);
  _runner = std::thread([this]() {
    pthread_setname_np(pthread_self(), "tx-idx-loop");
    run();
  });

  return {ErrorCodes::ERR_OK, ""};
}

Status IndexManager::scanExpiredKeysJob(uint32_t storeId) {
  bool expected = false;
  if (!_scanJobStatus[storeId].compare_exchange_strong(
        expected, true, std::memory_order_acq_rel)) {
    return {ErrorCodes::ERR_OK, ""};
  }

  if (_disableStatus[storeId].load(std::memory_order_relaxed)) {
    return {ErrorCodes::ERR_OK, ""};
  }

  auto guard = MakeGuard([this, storeId]() {
    _scanJobCnt[storeId]--;
    _scanJobStatus[storeId].store(false, std::memory_order_release);
  });

  bool clusterEnabled = _svr->getParams()->clusterEnabled;
  if (clusterEnabled && _svr->getMigrateManager()->existMigrateTask()) {
    return {ErrorCodes::ERR_OK, ""};
  }

  _scanJobCnt[storeId]++;
  LocalSessionGuard sg(_svr.get());
  auto expd = _svr->getSegmentMgr()->getDb(
    sg.getSession(), storeId, mgl::LockMode::LOCK_IS, true);
  if (!expd.ok()) {
    return expd.status();
  }

  PStore store = expd.value().store;
  // do nothing when it's a slave
  if (store->getMode() == KVStore::StoreMode::REPLICATE_ONLY ||
      !store->isOpen()) {
    return {ErrorCodes::ERR_OK, ""};
  }

  auto ptxn = store->createTransaction(sg.getSession());
  if (!ptxn.ok()) {
    return ptxn.status();
  }

  std::unique_ptr<Transaction> txn = std::move(ptxn.value());
  // Here, it's safe to use msSinceEpoch(), because it can't be a
  // slave here. In fact, it maybe more safe to use
  // store->getCurrentTime()
  auto cursor = txn->createTTLIndexCursor(store->getCurrentTime());
  INVARIANT(_scanPoints.find(storeId) != _scanPoints.end());
  // seek to the place where we left NOTE: skip the entry
  // already push into list
  std::string prefix;
  {
    std::lock_guard<std::mutex> lk(_mutex);
    prefix = _scanPoints[storeId];
  }

  if (prefix.size() > 0) {
    cursor->seek(prefix);
    auto key = cursor->key();
    if (!key.ok()) {
      return {ErrorCodes::ERR_OK, ""};
    }
    if (!prefix.compare(key.value())) {
      cursor->next();
    }
    // we need to firstly check whether there is ttl index in the
    // defalut colum_family
  }

  // TODO(takenliu) _scanPoints has error, _expiredKeys[storeId] will be
  // pushed back twice
  while (true) {
    auto record = cursor->next();
    if (!record.ok()) {
      // if no ttl index, or if ttl index not expired
      // scan again from _scanPoints[storeId] again
      // and we always resume from where we left, which
      // is remembered by _scanPoints[storeI]
      //
      // here's the invariant: if a ttl index T was picked
      // up by the scanner (which means its associate
      // key is expired), any attempt to inserting an ttl
      // index before T will result in a deletion of the
      // key.
      break;
    }

    {
      std::lock_guard<std::mutex> lk(_mutex);
      _scanPoints[storeId].assign(record.value().encode());
      _expiredKeys[storeId].push_back(std::move(record.value()));
      _totalEnqueue++;
      if (_expiredKeys[storeId].size() == _scanBatch) {
        break;
      }
    }

    TEST_SYNC_POINT_CALLBACK("InspectTotalEnqueue", &_totalEnqueue);
    TEST_SYNC_POINT_CALLBACK("InspectScanJobCnt", &_scanJobCnt[storeId]);
  }

  return {ErrorCodes::ERR_OK, ""};
}

Status IndexManager::stopStore(uint32_t storeId) {
  std::lock_guard<std::mutex> lk(_mutex);

  _expiredKeys[storeId].clear();

  _scanPoints[storeId] = std::move(std::string());
  _scanJobCnt[storeId] = {0u};
  _delJobCnt[storeId] = {0u};
  _disableStatus[storeId].store(true, std::memory_order_relaxed);

  return {ErrorCodes::ERR_OK, ""};
}

int IndexManager::tryDelExpiredKeysJob(uint32_t storeId) {
  bool expect = false;
  if (!_delJobStatus[storeId].compare_exchange_strong(
        expect, true, std::memory_order_acq_rel)) {
    return 0;
  }

  if (_disableStatus[storeId].load(std::memory_order_relaxed)) {
    return 0;
  }

  _delJobCnt[storeId]++;
  uint32_t deletes = 0;

  while (true) {
    TTLIndex index;

    {
      std::lock_guard<std::mutex> lk(_mutex);
      if (_expiredKeys[storeId].empty()) {
        break;
      }
      index = _expiredKeys[storeId].front();
    }
    LocalSessionGuard sg(_svr.get());
    auto sess = sg.getSession();
    sess->getCtx()->setAuthed();
    sess->getCtx()->setDbId(index.getDbId());
    Command::expireKeyIfNeeded(
      sg.getSession(), index.getPriKey(), index.getType());

    {
      std::lock_guard<std::mutex> lk(_mutex);
      INVARIANT(!_expiredKeys[storeId].empty());
      _expiredKeys[storeId].pop_front();
      _totalDequeue++;
      deletes++;
    }

    // break if delete a number of keys in the current store
    if (deletes == _delBatch) {
      break;
    }

    TEST_SYNC_POINT_CALLBACK("InspectTotalDequeue", &_totalDequeue);
    TEST_SYNC_POINT_CALLBACK("InspectDelJobCnt", &_delJobCnt[storeId]);
  }

  _delJobCnt[storeId]--;
  _delJobStatus[storeId].store(false, std::memory_order_release);
  return deletes;
}

// call this in a forever loop
Status IndexManager::run() {
  auto scheScanExpired = [this]() {
    for (uint32_t i = 0; i < _svr->getKVStoreCount(); ++i) {
      _indexScanner->schedule([this, i]() { scanExpiredKeysJob(i); });
    }
  };

  auto schedDelExpired = [this]() {
    std::vector<uint32_t> stored_with_expires;

    {
      std::lock_guard<std::mutex> lk(_mutex);
      for (uint32_t i = 0; i < _svr->getKVStoreCount(); ++i) {
        if (_expiredKeys[i].size() > 0) {
          stored_with_expires.push_back(i);
        }
      }
    }

    for (auto store_idx : stored_with_expires) {
      _keyDeleter->schedule(
        [this, store_idx]() { tryDelExpiredKeysJob(store_idx); });
    }

    return stored_with_expires.size() > 0;
  };
  LOG(WARNING) << "index manager running...";

  TEST_SYNC_POINT_CALLBACK("BeforeIndexManagerLoop", &_isRunning);
  while (_isRunning.load(std::memory_order_relaxed)) {
    scheScanExpired();
    schedDelExpired();
    std::this_thread::sleep_for(std::chrono::seconds(_pauseTime));
  }

  LOG(WARNING) << "index manager exiting...";

  return {ErrorCodes::ERR_OK, ""};
}

void IndexManager::stop() {
  LOG(WARNING) << "index manager begins to stop...";
  _isRunning.store(false, std::memory_order_relaxed);
  _runner.join();
  _indexScanner->stop();
  _keyDeleter->stop();
  LOG(WARNING) << "index manager stopped...";
}

bool IndexManager::isRunning() {
  return _isRunning.load(std::memory_order_relaxed);
}
}  // namespace tendisplus
