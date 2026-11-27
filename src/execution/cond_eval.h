// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#pragma once

#include <cstring>
#include <string>

#include "common/common.h"
#include "common/errors.h"
#include "executor_abstract.h"
#include "system/sm_meta.h"

namespace rucbase::exec {

/**
 * @brief 比较两个字段的原始字节内容。
 *
 * @param type 字段类型。
 * @param left / left_len 左字段起始地址与长度。
 * @param right / right_len 右字段起始地址与长度。
 * @return 负数 / 0 / 正数，与 operator<=> 语义一致。
 * @note CHAR(n) 字段按“去尾零后比较，等前缀时短者更小”的语义比较，
 *       与客户端解码后的 std::string 比较结果一致；两个字段的 n 可以不同。
 */
inline int compare_cells(ColType type, const char* left, int left_len, const char* right, int right_len) {
    switch (type) {
        case TYPE_INT: {
            int left_value;
            int right_value;
            memcpy(&left_value, left, sizeof(left_value));
            memcpy(&right_value, right, sizeof(right_value));
            return (left_value < right_value) ? -1 : (left_value > right_value) ? 1 : 0;
        }
        case TYPE_FLOAT: {
            float left_value;
            float right_value;
            memcpy(&left_value, left, sizeof(left_value));
            memcpy(&right_value, right, sizeof(right_value));
            return (left_value < right_value) ? -1 : (left_value > right_value) ? 1 : 0;
        }
        case TYPE_STRING: {
            const size_t left_len_trimmed = strnlen(left, static_cast<size_t>(left_len));
            const size_t right_len_trimmed = strnlen(right, static_cast<size_t>(right_len));
            const size_t common = std::min(left_len_trimmed, right_len_trimmed);
            const int cmp = memcmp(left, right, common);
            if (cmp != 0) {
                return (cmp < 0) ? -1 : 1;
            }
            return (left_len_trimmed < right_len_trimmed) ? -1 : (left_len_trimmed > right_len_trimmed) ? 1 : 0;
        }
    }
    throw InternalError("Unexpected column type in comparison");
}

/**
 * @brief 判断一条记录是否满足单个条件。
 *
 * @param rec_cols 记录字段布局；条件两侧的列都必须在其中（连接场景下是两侧字段的并集）。
 * @param cond 待判断的条件；右端可以是列，也可以是常量值。
 * @param rec 记录缓冲区的起始地址。
 */
inline bool eval_cond(const std::vector<ColMeta>& rec_cols, const Condition& cond, const char* rec) {
    const auto lhs = AbstractExecutor::get_col(rec_cols, cond.lhs_col);

    int cmp = 0;
    if (cond.is_rhs_val) {
        const Value& val = cond.rhs_val;
        if (val.type != lhs->type) {
            throw IncompatibleTypeError(coltype2str(lhs->type), coltype2str(val.type));
        }
        const char* lhs_buf = rec + lhs->offset;
        switch (lhs->type) {
            case TYPE_INT: {
                const int lhs_value = [&] {
                    int v;
                    memcpy(&v, lhs_buf, sizeof(v));
                    return v;
                }();
                cmp = (lhs_value < val.int_val) ? -1 : (lhs_value > val.int_val) ? 1 : 0;
                break;
            }
            case TYPE_FLOAT: {
                const float lhs_value = [&] {
                    float v;
                    memcpy(&v, lhs_buf, sizeof(v));
                    return v;
                }();
                cmp = (lhs_value < val.float_val) ? -1 : (lhs_value > val.float_val) ? 1 : 0;
                break;
            }
            case TYPE_STRING: {
                const size_t lhs_len_trimmed = strnlen(lhs_buf, static_cast<size_t>(lhs->len));
                const size_t rhs_len_trimmed = val.str_val.size();
                const size_t common = std::min(lhs_len_trimmed, rhs_len_trimmed);
                const int byte_cmp = memcmp(lhs_buf, val.str_val.data(), common);
                if (byte_cmp != 0) {
                    cmp = (byte_cmp < 0) ? -1 : 1;
                } else {
                    cmp = (lhs_len_trimmed < rhs_len_trimmed) ? -1 : (lhs_len_trimmed > rhs_len_trimmed) ? 1 : 0;
                }
                break;
            }
        }
    } else {
        const auto rhs = AbstractExecutor::get_col(rec_cols, cond.rhs_col);
        if (rhs->type != lhs->type) {
            throw IncompatibleTypeError(coltype2str(lhs->type), coltype2str(rhs->type));
        }
        cmp = compare_cells(lhs->type, rec + lhs->offset, lhs->len, rec + rhs->offset, rhs->len);
    }

    switch (cond.op) {
        case OP_EQ:
            return cmp == 0;
        case OP_NE:
            return cmp != 0;
        case OP_LT:
            return cmp < 0;
        case OP_GT:
            return cmp > 0;
        case OP_LE:
            return cmp <= 0;
        case OP_GE:
            return cmp >= 0;
    }
    throw InternalError("Unexpected comparison operator");
}

}  // namespace rucbase::exec
