// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#pragma once

#include <condition_variable>
#include <list>
#include <mutex>
#include <unordered_map>

#include "common/config.h"
#include "common/defs.h"
#include "transaction/txn_defs.h"

class Transaction;

inline constexpr const char* GroupLockModeStr[] = {"NON_LOCK", "IS", "IX", "S", "X", "SIX"};

class LockManager {
    /* 加锁类型，包括共享锁、排他锁、意向共享锁、意向排他锁、SIX（意向排他锁+共享锁） */
    enum class LockMode { SHARED, EXCLUSIVE, INTENTION_SHARED, INTENTION_EXCLUSIVE, S_IX };

    /* 用于标识加锁队列中排他性最强的锁类型，例如加锁队列中有SHARED和EXLUSIVE两个加锁操作，则该队列的锁模式为X */
    enum class GroupLockMode { NON_LOCK, IS, IX, S, X, SIX };

    /* 事务的加锁申请 */
    class LockRequest {
    public:
        LockRequest(txn_id_t txn_id, LockMode lock_mode) : txn_id_(txn_id), lock_mode_(lock_mode), granted_(false) {}

        txn_id_t txn_id_;     // 申请加锁的事务ID
        LockMode lock_mode_;  // 事务申请加锁的类型
        bool granted_;        // 该事务是否已经被赋予锁
    };

    /* 数据项上的加锁队列 */
    class LockRequestQueue {
    public:
        std::list<LockRequest> request_queue_;  // 加锁队列
        // 条件变量，用于唤醒正在等待加锁的申请，在no-wait策略下无需使用
        std::condition_variable cv_;
        GroupLockMode group_lock_mode_ = GroupLockMode::NON_LOCK;  // 加锁队列的锁模式
    };

public:
    LockManager() = default;

    ~LockManager() = default;

    bool lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd);

    bool lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd);

    bool lock_shared_on_table(Transaction* txn, int tab_fd);

    bool lock_exclusive_on_table(Transaction* txn, int tab_fd);

    bool lock_IS_on_table(Transaction* txn, int tab_fd);

    bool lock_IX_on_table(Transaction* txn, int tab_fd);

    bool unlock(Transaction* txn, LockDataId lock_data_id);

private:
    /** @brief 统一的加锁流程：相容性检查 / 升级 / 记录锁，no-wait 语义。 */
    bool lock_common(Transaction* txn, LockDataId lock_data_id, LockMode lock_mode);

    /** @brief 判断已持有的锁是否满足（不弱于）本次申请。 */
    static bool holds_sufficient(LockMode held, LockMode requested);

    /** @brief 判断申请模式与锁队列当前组模式是否相容。 */
    static bool is_compatible(LockMode requested, GroupLockMode group);

    /** @brief 授予一个新锁后更新队列的组模式。 */
    static GroupLockMode combine(GroupLockMode current, LockMode granted);

    /** @brief 释放一个锁后根据剩余请求重新计算组模式。 */
    static GroupLockMode recompute(const LockRequestQueue& queue);

    std::mutex latch_;                                             // 用于锁表的并发
    std::unordered_map<LockDataId, LockRequestQueue> lock_table_;  // 全局锁表
};
