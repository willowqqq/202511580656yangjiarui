// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/common.h"
#include "common/context.h"
#include "common/defs.h"
#include "common/errors.h"
#include "executor_abstract.h"
#include "index/index_manager.h"
#include "record/rm_defs.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"
#include "system/sm_meta.h"
#include "transaction/concurrency/lock_manager.h"
#include "transaction/transaction.h"

class UpdateExecutor : public AbstractExecutor {
private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle* fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager* sm_manager_;

public:
    UpdateExecutor(SmManager* sm_manager,
                   const std::string& tab_name,
                   std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds,
                   std::vector<Rid> rids,
                   Context* context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = std::move(set_clauses);
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = std::move(conds);
        rids_ = std::move(rids);
        context_ = context;
    }
    /**
     * @brief 按 rids_ 逐条更新记录，并维护表上的全部索引。
     * @throws DuplicateKeyError 新值与某个唯一索引中已有的 key 冲突。抛出前，发生冲突的这一行
     *         （记录本身以及它在所有索引中的项）必须恢复原状。
     * @note 把一行的唯一键更新为它原来的值不算冲突。
     */
    std::unique_ptr<RmRecord> next() override {
        Transaction* txn = context_ == nullptr ? nullptr : context_->transaction();
        for (const Rid& rid : rids_) {
            // 先申请行级写锁：失败则本行尚未发生任何修改（含索引项），可以直接回滚事务
            if (txn != nullptr &&
                !context_->lock_manager()->lock_exclusive_on_record(txn, rid, fh_->fd())) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            auto old_rec = fh_->get_record(rid, context_);

            // 构造新记录：先整体拷贝旧记录，再按 set_clauses_ 覆盖对应字段
            auto new_rec = std::make_unique<RmRecord>(old_rec->size, old_rec->data);
            for (const auto& set_clause : set_clauses_) {
                const auto col = std::ranges::find_if(tab_.cols, [&](const ColMeta& c) {
                    return c.name == set_clause.lhs.col_name && c.tab_name == set_clause.lhs.tab_name;
                });
                if (col == tab_.cols.end()) {
                    throw ColumnNotFoundError(set_clause.lhs.col_name);
                }
                Value val = set_clause.rhs;
                if (val.type != col->type) {
                    throw IncompatibleTypeError(coltype2str(col->type), coltype2str(val.type));
                }
                if (val.raw == nullptr) {
                    val.init_raw(col->len);
                } else if (val.raw->size != col->len) {
                    throw InternalError("Update value raw size does not match column length");
                }
                memcpy(new_rec->data + col->offset, val.raw->data, static_cast<size_t>(col->len));
            }

            // 唯一约束检查：先于任何修改进行，保证冲突时这一行保持原状。
            // 新 key 与原 key 相同不算冲突。
            for (const auto& index : tab_.indexes) {
                if (!index.unique) {
                    continue;
                }
                std::vector<char> old_key = index.make_key(old_rec->data);
                std::vector<char> new_key = index.make_key(new_rec->data);
                if (old_key == new_key) {
                    continue;
                }
                const std::string index_name = sm_manager_->get_index_manager()->make_index_name(tab_name_, index.cols);
                BPlusTree* ih = sm_manager_->indexes_.at(index_name).get();
                std::vector<Rid> existing;
                if (ih->get_value(new_key.data(), &existing, txn) && !existing.empty()) {
                    throw DuplicateKeyError();
                }
            }

            // 先删旧索引项，再写记录，最后插新索引项
            for (const auto& index : tab_.indexes) {
                const std::string index_name = sm_manager_->get_index_manager()->make_index_name(tab_name_, index.cols);
                BPlusTree* ih = sm_manager_->indexes_.at(index_name).get();
                std::vector<char> old_key = index.make_key(old_rec->data);
                std::vector<char> new_key = index.make_key(new_rec->data);
                if (old_key == new_key) {
                    continue;  // key 未变，索引项无需动
                }
                ih->delete_entry(old_key.data(), rid, txn);
                ih->insert_entry(new_key.data(), rid, txn);
            }
            fh_->update_record(rid, new_rec->data, context_);
        }
        return nullptr;
    }

    Rid& rid() override { return abstract_rid_; }
};
