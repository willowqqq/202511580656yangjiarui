// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "transaction_manager.h"

#include <vector>

#include "concurrency/lock_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"
#include "transaction/transaction.h"

namespace {

/**
 * @brief 释放事务持有的全部锁。
 * @note 先拷贝锁集合再逐个释放，unlock 会修改 txn->lock_set()。
 */
void release_all_locks(LockManager* lock_manager, Transaction* txn) {
    std::vector<LockDataId> lock_ids(txn->lock_set().begin(), txn->lock_set().end());
    for (const auto& lock_id : lock_ids) {
        lock_manager->unlock(txn, lock_id);
    }
}

}  // namespace

/**
 * @brief 创建新事务并由 *this 独占。
 * @note 不要从外部传入 Transaction*。所有事务都必须由本函数 make_unique 后放入 txn_map_。
 */
Transaction* TransactionManager::begin(LogManager* log_manager) {
    const txn_id_t txn_id = next_txn_id_++;
    auto txn = std::make_unique<Transaction>(txn_id);
    txn->set_state(TransactionState::GROWING);
    txn->set_start_ts(next_timestamp_++);

    std::unique_lock<std::mutex> lock(latch_);
    Transaction* raw = txn.get();
    txn_map_.emplace(txn_id, std::move(txn));
    return raw;
}

/**
 * @brief 事务的提交方法。
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // 释放全部锁（两阶段封锁的收缩点），清理写集，再从表中移除事务
    release_all_locks(lock_manager_, txn);
    txn->write_set().clear();
    txn->set_state(TransactionState::COMMITTED);

    std::unique_lock<std::mutex> lock(latch_);
    txn_map_.erase(txn->get_transaction_id());
}

/**
 * @brief 事务的终止（回滚）方法。
 */
void TransactionManager::abort(Transaction* txn, LogManager* log_manager) {
    // 按相反顺序撤销写操作：同一记录被连续更新多次时，只有逆序恢复才能回到事务开始前的值
    while (!txn->write_set().empty()) {
        WriteRecord record = std::move(txn->write_set().back());
        txn->write_set().pop_back();

        const auto handle_it = sm_manager_->fhs_.find(record.table_name());
        if (handle_it == sm_manager_->fhs_.end()) {
            continue;  // 表已被删除等极端情况，跳过该条写操作
        }
        RmFileHandle* fh = handle_it->second.get();
        switch (record.write_type()) {
            case WType::INSERT_TUPLE:
                fh->delete_record(record.rid(), nullptr);
                break;
            case WType::DELETE_TUPLE:
                // 回滚删除：把旧记录插回原位置（rid 不变）
                fh->insert_record(record.rid(), record.record().data);
                break;
            case WType::UPDATE_TUPLE:
                fh->update_record(record.rid(), record.record().data, nullptr);
                break;
        }
    }

    release_all_locks(lock_manager_, txn);
    txn->set_state(TransactionState::ABORTED);

    std::unique_lock<std::mutex> lock(latch_);
    txn_map_.erase(txn->get_transaction_id());
}
