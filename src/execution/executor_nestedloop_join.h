// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "common/common.h"
#include "common/defs.h"
#include "common/errors.h"
#include "cond_eval.h"
#include "executor_abstract.h"
#include "record/rm_defs.h"
#include "system/sm_meta.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> left_;   // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;  // 右儿子节点（需要join的表）
    size_t len_;                               // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                // join后获得的记录的字段

    std::vector<Condition> fed_conds_;  // join条件
    bool is_end_;

public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                           std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tuple_len() + right_->tuple_len();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto& col : right_cols) {
            col.offset += static_cast<int>(left_->tuple_len());
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        is_end_ = false;
        fed_conds_ = std::move(conds);
    }

    void begin_tuple() override {
        left_->begin_tuple();
        right_->begin_tuple();
        is_end_ = !find_match();
    }

    void next_tuple() override {
        if (is_end_) {
            return;
        }
        // 当前组合已匹配：先推进右孩子，再继续寻找下一个匹配组合
        right_->next_tuple();
        is_end_ = !find_match();
    }

    bool is_end() const override { return is_end_; }

    std::unique_ptr<RmRecord> next() override {
        auto left_rec = left_->next();
        auto right_rec = right_->next();
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        memcpy(rec->data, left_rec->data, left_->tuple_len());
        memcpy(rec->data + left_->tuple_len(), right_rec->data, right_->tuple_len());
        return rec;
    }

    size_t tuple_len() const override { return len_; }

    const std::vector<ColMeta>& cols() const override { return cols_; }

    Rid& rid() override { return abstract_rid_; }

    /**
     * @brief 从当前左右组合开始，找到下一个满足全部连接条件的组合。
     *
     * 左孩子每前进一条，右孩子都要从头重扫一遍（嵌套循环的语义）。
     * @return 是否找到匹配组合。
     */
    bool find_match() {
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                if (check_conds()) {
                    return true;
                }
                right_->next_tuple();
            }
            left_->next_tuple();
            if (!left_->is_end()) {
                right_->begin_tuple();
            }
        }
        return false;
    }

    /** @brief 判断当前左右组合是否满足全部连接条件。 */
    bool check_conds() {
        auto left_rec = left_->next();
        auto right_rec = right_->next();
        auto joined = std::make_unique<RmRecord>(static_cast<int>(len_));
        memcpy(joined->data, left_rec->data, left_->tuple_len());
        memcpy(joined->data + left_->tuple_len(), right_rec->data, right_->tuple_len());
        for (const auto& cond : fed_conds_) {
            if (!rucbase::exec::eval_cond(cols_, cond, joined->data)) {
                return false;
            }
        }
        return true;
    }
};
