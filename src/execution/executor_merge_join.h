// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#pragma once

#include <cstddef>
#include <iterator>
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
 * @brief 排序归并连接（Sort-Merge Join）。
 *
 * 左右孩子的输出都已按各自的连接键升序排列（Planner 会在两侧放置 SortExecutor）。
 * 算子同时推进左右两侧，只输出连接键相等、且满足其余连接条件的记录对。
 * 输出记录的布局与嵌套循环连接相同：左孩子记录在前，右孩子记录在后。
 *
 * @note 两侧都可能出现连接键重复的记录。火山模型的孩子算子只能向前迭代，不能回退。
 */
class MergeJoinExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> left_;   // 左孩子，按 left_key_ 升序输出
    std::unique_ptr<AbstractExecutor> right_;  // 右孩子，按 right_key_ 升序输出
    size_t len_;                               // 连接后每条记录的长度
    std::vector<ColMeta> cols_;                // 连接后记录的字段；右孩子字段的 offset 已加上左记录长度

    Condition merge_cond_;                   // 归并键：left_key_ = right_key_
    ColMeta left_key_;                       // 连接键在左孩子记录中的位置
    ColMeta right_key_;                      // 连接键在右孩子记录中的位置
    std::vector<Condition> residual_conds_;  // 其余连接条件，在连接键相等后继续检查
    // 请自行添加归并过程所需的状态。
    // 火山模型的孩子只能向前迭代：两侧各维护“当前 key 的整组记录”以及
    // “读到的第一条不同 key 的记录”（pending），避免为了分组而越过边界后无法回退。
    std::vector<std::unique_ptr<RmRecord>> left_group_;
    std::vector<std::unique_ptr<RmRecord>> right_group_;
    std::unique_ptr<RmRecord> left_pending_;
    std::unique_ptr<RmRecord> right_pending_;
    size_t pair_idx_ = 0;        // 当前组内输出到第几个组合（按 左组×右组 展开）
    bool keys_equal_ = false;    // 两侧当前组的 key 是否已经确认相等（相等才允许输出组合）
    std::unique_ptr<RmRecord> current_;  // 当前输出记录
    bool is_end_ = false;

public:
    /**
     * @param conds conds[0] 为归并键（列与列的 OP_EQ，lhs 来自左孩子、rhs 来自右孩子），其余为附加条件。
     * @throws InternalError conds 为空或 conds[0] 不是列与列的等值条件。
     */
    MergeJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                      std::unique_ptr<AbstractExecutor> right,
                      std::vector<Condition> conds) {
        if (conds.empty() || conds.front().is_rhs_val || conds.front().op != OP_EQ) {
            throw InternalError("MergeJoinExecutor requires a column equality as its first condition");
        }
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tuple_len() + right_->tuple_len();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto& col : right_cols) {
            col.offset += static_cast<int>(left_->tuple_len());
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());

        merge_cond_ = conds.front();
        left_key_ = *get_col(left_->cols(), merge_cond_.lhs_col);
        right_key_ = *get_col(right_->cols(), merge_cond_.rhs_col);
        residual_conds_.assign(std::make_move_iterator(conds.begin() + 1), std::make_move_iterator(conds.end()));
    }

    void begin_tuple() override {
        left_->begin_tuple();
        right_->begin_tuple();
        left_group_.clear();
        right_group_.clear();
        left_pending_.reset();
        right_pending_.reset();
        pair_idx_ = 0;
        keys_equal_ = false;
        is_end_ = false;
        advance_to_next_match();
    }

    void next_tuple() override {
        if (is_end_) {
            return;
        }
        ++pair_idx_;
        advance_to_next_match();
    }

    bool is_end() const override { return is_end_; }

    std::unique_ptr<RmRecord> next() override {
        return std::make_unique<RmRecord>(static_cast<int>(len_), current_->data);
    }

    size_t tuple_len() const override { return len_; }

    const std::vector<ColMeta>& cols() const override { return cols_; }

    Rid& rid() override { return abstract_rid_; }

private:
    /** @brief 提取记录中连接键的原始字节。 */
    static std::vector<char> key_of(const ColMeta& key_col, const RmRecord* rec) {
        std::vector<char> key(static_cast<size_t>(key_col.len));
        memcpy(key.data(), rec->data + key_col.offset, static_cast<size_t>(key_col.len));
        return key;
    }

    /**
     * @brief 加载孩子中连接键相同的一组记录到 group 中。
     *
     * 进入时：pending 非空则它属于该组（孩子已越过它），否则孩子正指向组内第一条。
     * 退出时：group 含该 key 的全部记录，pending 为第一条不同 key 的记录（或为空）。
     */
    static void load_group(AbstractExecutor* child,
                           const ColMeta& key_col,
                           std::vector<std::unique_ptr<RmRecord>>* group,
                           std::unique_ptr<RmRecord>* pending) {
        group->clear();
        if (*pending != nullptr) {
            group->push_back(std::move(*pending));
        } else {
            group->push_back(child->next());
        }
        const std::vector<char> key = key_of(key_col, group->front().get());
        while (true) {
            child->next_tuple();
            if (child->is_end()) {
                break;
            }
            auto rec = child->next();
            if (key_of(key_col, rec.get()) != key) {
                *pending = std::move(rec);
                break;
            }
            group->push_back(std::move(rec));
        }
    }

    /** @brief 当前组合是否满足全部附加连接条件。 */
    bool residuals_hold(size_t left_idx, size_t right_idx) {
        auto joined = std::make_unique<RmRecord>(static_cast<int>(len_));
        memcpy(joined->data, left_group_[left_idx]->data, left_->tuple_len());
        memcpy(joined->data + left_->tuple_len(), right_group_[right_idx]->data, right_->tuple_len());
        for (const auto& cond : residual_conds_) {
            if (!rucbase::exec::eval_cond(cols_, cond, joined->data)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 归并推进，直到找到下一个满足条件的组合并写入 current_，或确定无更多结果。
     *
     * 归并规则：左侧组 key 小于右侧组 key 时丢弃左组；反之丢弃右组；相等时输出
     * 两组的笛卡尔积中满足附加条件的组合，输出完后两侧都越过该 key。
     */
    void advance_to_next_match() {
        while (true) {
            // 1) 两侧当前组的 key 已确认相等：输出组内组合（跳过残差条件不满足的组合）
            if (keys_equal_) {
                if (pair_idx_ < left_group_.size() * right_group_.size()) {
                    const size_t left_idx = pair_idx_ / right_group_.size();
                    const size_t right_idx = pair_idx_ % right_group_.size();
                    if (residuals_hold(left_idx, right_idx)) {
                        auto joined = std::make_unique<RmRecord>(static_cast<int>(len_));
                        memcpy(joined->data, left_group_[left_idx]->data, left_->tuple_len());
                        memcpy(joined->data + left_->tuple_len(), right_group_[right_idx]->data, right_->tuple_len());
                        current_ = std::move(joined);
                        return;
                    }
                    ++pair_idx_;
                    continue;
                }
                // 该 key 的全部组合已输出（或全部被残差条件过滤）：两侧越过这个 key
                left_group_.clear();
                right_group_.clear();
                pair_idx_ = 0;
                keys_equal_ = false;
            }

            // 2) 补全缺失的一侧（组被丢弃或已越过时）。pending 中的记录优先作为新组的第一条。
            if (left_group_.empty()) {
                if (left_pending_ == nullptr && left_->is_end()) {
                    is_end_ = true;
                    return;
                }
                load_group(left_.get(), left_key_, &left_group_, &left_pending_);
            }
            if (right_group_.empty()) {
                if (right_pending_ == nullptr && right_->is_end()) {
                    is_end_ = true;
                    return;
                }
                load_group(right_.get(), right_key_, &right_group_, &right_pending_);
            }

            // 3) 比较两侧当前组的 key：小的那一侧丢弃，下一轮循环重新加载；相等则标记并进入组内输出
            const int cmp = rucbase::exec::compare_cells(
                left_key_.type, left_group_.front()->data + left_key_.offset, left_key_.len,
                right_group_.front()->data + right_key_.offset, right_key_.len);
            if (cmp < 0) {
                left_group_.clear();
            } else if (cmp > 0) {
                right_group_.clear();
            } else {
                keys_equal_ = true;
            }
        }
    }
};
