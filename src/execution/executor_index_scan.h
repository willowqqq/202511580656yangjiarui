// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#pragma once

#include <cassert>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/common.h"
#include "common/config.h"
#include "common/context.h"
#include "common/defs.h"
#include "common/errors.h"
#include "cond_eval.h"
#include "executor_abstract.h"
#include "index/b_plus_tree.h"
#include "index/index_manager.h"
#include "index/index_scan.h"
#include "record/rm_defs.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"
#include "system/sm_meta.h"
#include "transaction/concurrency/lock_manager.h"
#include "transaction/transaction.h"

class IndexScanExecutor : public AbstractExecutor {
private:
    std::string tab_name_;              // 表名称
    TabMeta tab_;                       // 表的元数据
    std::vector<Condition> conds_;      // 扫描条件
    RmFileHandle* fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // 需要读取的字段
    size_t len_;                        // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;  // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_{.page_no = INVALID_PAGE_ID, .slot_no = -1};
    std::unique_ptr<RecScan> scan_;

    SmManager* sm_manager_;

public:
    IndexScanExecutor(SmManager* sm_manager,
                      std::string tab_name,
                      std::vector<Condition> conds,
                      std::vector<std::string> index_col_names,
                      Context* context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        // 按值形参 + move 到成员：避免再拷贝一份 vector。
        index_col_names_ = std::move(index_col_names);
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto& cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void begin_tuple() override {
        BPlusTree* tree =
            sm_manager_->indexes_.at(sm_manager_->get_index_manager()->make_index_name(tab_name_, index_col_names_))
                .get();

        // 在第一个索引列上找右侧为常量的条件，据此确定扫描区间；找不到则全表范围
        const Condition* range_cond = nullptr;
        for (const auto& cond : conds_) {
            if (cond.is_rhs_val && cond.lhs_col.tab_name == tab_name_ &&
                cond.lhs_col.col_name == index_col_names_.front()) {
                range_cond = &cond;
                break;
            }
        }

        IndexPosition begin;
        IndexPosition end;
        if (range_cond == nullptr) {
            begin = tree->leaf_begin();
            end = tree->leaf_end();
        } else {
            // 构造边界键：首列取条件值，其余列按类型填充最小值（下界）与最大值（上界）。
            // 注意 int 比较是有符号的：0xFF 字节代表 -1，不能当作上界填充。
            const ColMeta& first_col = index_meta_.cols.front();
            Value value = range_cond->rhs_val;
            if (value.type != first_col.type) {
                throw IncompatibleTypeError(coltype2str(first_col.type), coltype2str(value.type));
            }
            if (value.raw == nullptr) {
                value.init_raw(first_col.len);
            }
            std::vector<char> low(index_meta_.col_tot_len, 0);
            std::vector<char> high(index_meta_.col_tot_len, static_cast<char>(0xFF));
            memcpy(low.data(), value.raw->data, static_cast<size_t>(first_col.len));
            memcpy(high.data(), value.raw->data, static_cast<size_t>(first_col.len));
            int offset = first_col.len;
            for (size_t i = 1; i < index_meta_.cols.size(); ++i) {
                const ColMeta& col = index_meta_.cols[i];
                switch (col.type) {
                    case TYPE_INT: {
                        const int int_max = std::numeric_limits<int>::max();
                        const int int_min = std::numeric_limits<int>::min();
                        memcpy(high.data() + offset, &int_max, sizeof(int_max));
                        memcpy(low.data() + offset, &int_min, sizeof(int_min));
                        break;
                    }
                    case TYPE_FLOAT: {
                        const float float_max = std::numeric_limits<float>::max();
                        const float float_min = -std::numeric_limits<float>::max();
                        memcpy(high.data() + offset, &float_max, sizeof(float_max));
                        memcpy(low.data() + offset, &float_min, sizeof(float_min));
                        break;
                    }
                    case TYPE_STRING:
                        // char 字段按去尾零语义比较，0xFF 与 0x00 足以构成上下界
                        break;
                }
                offset += col.len;
            }

            switch (range_cond->op) {
                case OP_EQ:
                    begin = tree->lower_bound(low.data());
                    end = tree->upper_bound(high.data());
                    break;
                case OP_LT:
                    begin = tree->leaf_begin();
                    end = tree->lower_bound(low.data());
                    break;
                case OP_LE:
                    begin = tree->leaf_begin();
                    end = tree->upper_bound(high.data());
                    break;
                case OP_GT:
                    begin = tree->upper_bound(high.data());
                    end = tree->leaf_end();
                    break;
                case OP_GE:
                    begin = tree->lower_bound(low.data());
                    end = tree->leaf_end();
                    break;
                case OP_NE:
                    begin = tree->leaf_begin();
                    end = tree->leaf_end();
                    break;
            }
        }

        scan_ = std::make_unique<IndexScan>(tree, begin, end);

        // Lab4：表级 S 锁防止幻读（表锁方案）；行级锁由 get_record 负责
        if (context_ != nullptr && context_->transaction() != nullptr) {
            Transaction* txn = context_->transaction();
            if (!context_->lock_manager()->lock_shared_on_table(txn, fh_->fd())) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
        }
        advance_to_match();
    }

    void next_tuple() override {
        scan_->next();
        advance_to_match();
    }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }

    std::unique_ptr<RmRecord> next() override { return fh_->get_record(rid_, context_); }

    size_t tuple_len() const override { return len_; }

    const std::vector<ColMeta>& cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol& target) override { return *get_col(cols_, target); }

    /**
     * @brief 从 scan_ 当前位置起，找到下一条满足全部条件的索引项，并把记录位置写入 rid_。
     */
    void advance_to_match() {
        while (scan_ != nullptr && !scan_->is_end()) {
            const Rid rid = scan_->rid();
            auto rec = fh_->get_record(rid, context_);
            bool satisfied = true;
            for (const auto& cond : fed_conds_) {
                if (!rucbase::exec::eval_cond(cols_, cond, rec->data)) {
                    satisfied = false;
                    break;
                }
            }
            if (satisfied) {
                rid_ = rid;
                return;
            }
            scan_->next();
        }
        rid_ = {.page_no = INVALID_PAGE_ID, .slot_no = -1};
    }

    Rid& rid() override { return rid_; }
};
