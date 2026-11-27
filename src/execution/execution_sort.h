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

/**
 * @brief 按单个字段对子算子的输出排序。
 *
 * 用于 ORDER BY，也作为排序归并连接两侧的输入。输出记录的字段布局与子算子完全相同。
 * 框架只支持单个排序键；如需多键排序，可自行扩展数据结构。
 */
class SortExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_;  // 子算子
    ColMeta sort_col_;                        // 排序字段在子算子输出记录中的位置和类型
    bool is_desc_;                            // 是否降序
    // 提示：排序算子需要先读完子算子的全部输出，再按顺序返回。
    // 请自行添加保存记录和迭代位置所需的成员。
    std::vector<std::unique_ptr<RmRecord>> records_;  // 子算子全部输出的缓冲
    size_t position_ = 0;                             // 下一条要返回的记录下标
    size_t len_ = 0;
    std::vector<ColMeta> cols_;

public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, const TabCol& sel_cols, bool is_desc) {
        prev_ = std::move(prev);
        sort_col_ = *get_col(prev_->cols(), sel_cols);
        is_desc_ = is_desc;
        len_ = prev_->tuple_len();
        cols_ = prev_->cols();
    }

    void begin_tuple() override {
        // 先读完孩子的全部输出（只扫描一遍），再按 sort_col_ 排序
        records_.clear();
        for (prev_->begin_tuple(); !prev_->is_end(); prev_->next_tuple()) {
            records_.push_back(prev_->next());
        }
        const auto cmp = [this](const std::unique_ptr<RmRecord>& lhs, const std::unique_ptr<RmRecord>& rhs) {
            const int result = rucbase::exec::compare_cells(sort_col_.type, lhs->data + sort_col_.offset, sort_col_.len,
                                                            rhs->data + sort_col_.offset, sort_col_.len);
            return is_desc_ ? result > 0 : result < 0;
        };
        std::stable_sort(records_.begin(), records_.end(), cmp);
        position_ = 0;
    }

    void next_tuple() override {
        if (position_ < records_.size()) {
            ++position_;
        }
    }

    bool is_end() const override { return position_ >= records_.size(); }

    std::unique_ptr<RmRecord> next() override {
        return std::make_unique<RmRecord>(static_cast<int>(len_), records_[position_]->data);
    }

    size_t tuple_len() const override { return len_; }

    const std::vector<ColMeta>& cols() const override { return cols_; }

    Rid& rid() override { return abstract_rid_; }
};
