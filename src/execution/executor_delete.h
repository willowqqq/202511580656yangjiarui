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

class DeleteExecutor : public AbstractExecutor {
private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Condition> conds_;  // delete的条件
    RmFileHandle* fh_;              // 表的数据文件句柄
    std::vector<Rid> rids_;         // 需要删除的记录的位置
    std::string tab_name_;          // 表名称
    SmManager* sm_manager_;

public:
    DeleteExecutor(SmManager* sm_manager,
                   const std::string& tab_name,
                   std::vector<Condition> conds,
                   std::vector<Rid> rids,
                   Context* context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = std::move(conds);
        rids_ = std::move(rids);
        context_ = context;
    }

    /**
     * @brief 按 rids_ 删除记录，并按 (key, rid) 删除它在每个索引中的项。
     * @note 同一 key 在普通索引中可能对应多条记录，只能删掉属于当前记录的那一条。
     */
    std::unique_ptr<RmRecord> next() override {
        Transaction* txn = context_ == nullptr ? nullptr : context_->transaction();
        for (const Rid& rid : rids_) {
            // 先申请行级写锁：失败则本行尚未发生任何修改（含索引项），可以直接回滚事务
            if (txn != nullptr &&
                !context_->lock_manager()->lock_exclusive_on_record(txn, rid, fh_->fd())) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            auto rec = fh_->get_record(rid, context_);
            // 先删索引项，再删记录，保证两者一致
            for (const auto& index : tab_.indexes) {
                const std::string index_name = sm_manager_->get_index_manager()->make_index_name(tab_name_, index.cols);
                BPlusTree* ih = sm_manager_->indexes_.at(index_name).get();
                std::vector<char> key = index.make_key(rec->data);
                ih->delete_entry(key.data(), rid, txn);
            }
            fh_->delete_record(rid, context_);
        }
        return nullptr;
    }

    Rid& rid() override { return abstract_rid_; }
};
