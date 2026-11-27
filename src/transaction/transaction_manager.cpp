// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "transaction_manager.h"

#include <algorithm>
#include <vector>

#include "concurrency/lock_manager.h"
#include "index/b_plus_tree.h"
#include "index/index_manager.h"
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

/**
 * @brief 幂等地删除索引项：不存在时静默跳过，避免回滚路径上抛异常。
 */
void remove_index_entry(BPlusTree* tree, const char* key, const Rid& rid) {
    std::vector<Rid> rids;
    if (!tree->get_value(key, &rids, nullptr) ||
        std::find(rids.begin(), rids.end(), rid) == rids.end()) {
        return;
    }
    tree->delete_entry(key, rid, nullptr);
}

/**
 * @brief 幂等地插入索引项：已存在（key, rid）时静默跳过；唯一索引的 key 已被其他记录占用时也跳过，
 *        避免回滚路径上抛出 DuplicateKeyError 导致异常逃逸。
 */
void add_index_entry(BPlusTree* tree, const char* key, const Rid& rid) {
    std::vector<Rid> rids;
    if (tree->get_value(key, &rids, nullptr)) {
        return;
    }
    tree->insert_entry(key, rid, nullptr);
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
    // 按相反顺序撤销写操作：同一记录被连续更新多次时，只有逆序恢复才能回到事务开始前的值。
    // 表上有索引时，记录与索引项必须同步回滚。
    while (!txn->write_set().empty()) {
        WriteRecord record = std::move(txn->write_set().back());
        txn->write_set().pop_back();

        const auto handle_it = sm_manager_->fhs_.find(record.table_name());
        if (handle_it == sm_manager_->fhs_.end() || !sm_manager_->db_.is_table(record.table_name())) {
            continue;  // 表已被删除等极端情况，跳过该条写操作
        }
        RmFileHandle* fh = handle_it->second.get();
        const TabMeta& tab = sm_manager_->db_.get_table(record.table_name());
        const auto index_handle = [&](const IndexMeta& index) -> BPlusTree* {
            const auto it =
                sm_manager_->indexes_.find(sm_manager_->get_index_manager()->make_index_name(record.table_name(),
                                                                                              index.cols));
            return it == sm_manager_->indexes_.end() ? nullptr : it->second.get();
        };

        switch (record.write_type()) {
            case WType::INSERT_TUPLE: {
                // 撤销插入：先删索引项（需要记录内容），再删记录
                if (auto rec = fh->get_record(record.rid(), nullptr); rec != nullptr) {
                    for (const auto& index : tab.indexes) {
                        if (BPlusTree* tree = index_handle(index); tree != nullptr) {
                            std::vector<char> key = index.make_key(rec->data);
                            remove_index_entry(tree, key.data(), record.rid());
                        }
                    }
                }
                fh->delete_record(record.rid(), nullptr);
                break;
            }
            case WType::DELETE_TUPLE: {
                // 撤销删除：先恢复记录，再把索引项插回去
                fh->insert_record(record.rid(), record.record().data);
                for (const auto& index : tab.indexes) {
                    if (BPlusTree* tree = index_handle(index); tree != nullptr) {
                        std::vector<char> key = index.make_key(record.record().data);
                        add_index_entry(tree, key.data(), record.rid());
                    }
                }
                break;
            }
            case WType::UPDATE_TUPLE: {
                // 撤销更新：删新 key 索引项，恢复旧值，再插旧 key 索引项
                if (auto rec = fh->get_record(record.rid(), nullptr); rec != nullptr) {
                    for (const auto& index : tab.indexes) {
                        if (BPlusTree* tree = index_handle(index); tree != nullptr) {
                            std::vector<char> key = index.make_key(rec->data);
                            remove_index_entry(tree, key.data(), record.rid());
                        }
                    }
                }
                fh->update_record(record.rid(), record.record().data, nullptr);
                for (const auto& index : tab.indexes) {
                    if (BPlusTree* tree = index_handle(index); tree != nullptr) {
                        std::vector<char> key = index.make_key(record.record().data);
                        add_index_entry(tree, key.data(), record.rid());
                    }
                }
                break;
            }
        }
    }

    release_all_locks(lock_manager_, txn);
    txn->set_state(TransactionState::ABORTED);

    std::unique_lock<std::mutex> lock(latch_);
    txn_map_.erase(txn->get_transaction_id());
}
