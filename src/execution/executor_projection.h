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
#include "executor_abstract.h"
#include "record/rm_defs.h"
#include "system/sm_meta.h"

class ProjectionExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_;  // 投影节点的儿子节点
    std::vector<ColMeta> cols_;               // 需要投影的字段
    size_t len_;                              // 字段总长度
    std::vector<size_t> sel_idxs_;

public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol>& sel_cols) {
        prev_ = std::move(prev);

        int curr_offset = 0;
        auto& prev_cols = prev_->cols();
        for (auto& sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    void begin_tuple() override { prev_->begin_tuple(); }

    void next_tuple() override { prev_->next_tuple(); }

    bool is_end() const override { return prev_->is_end(); }

    std::unique_ptr<RmRecord> next() override {
        auto prev_rec = prev_->next();
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        for (size_t i = 0; i < cols_.size(); ++i) {
            const ColMeta& source = prev_->cols()[sel_idxs_[i]];
            const ColMeta& target = cols_[i];
            memcpy(rec->data + target.offset, prev_rec->data + source.offset, static_cast<size_t>(source.len));
        }
        return rec;
    }

    size_t tuple_len() const override { return len_; }

    const std::vector<ColMeta>& cols() const override { return cols_; }

    Rid& rid() override { return abstract_rid_; }
};
