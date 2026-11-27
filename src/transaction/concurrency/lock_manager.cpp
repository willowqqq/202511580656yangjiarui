// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "lock_manager.h"

#include <algorithm>

#include "transaction/transaction.h"

int LockManager::mode_strength(LockMode mode) {
    switch (mode) {
        case LockMode::EXCLUSIVE:
            return 4;
        case LockMode::S_IX:
            return 3;
        case LockMode::SHARED:
        case LockMode::INTENTION_EXCLUSIVE:
            return 2;
        case LockMode::INTENTION_SHARED:
            return 1;
    }
    return 0;
}

bool LockManager::holds_sufficient(LockMode held, LockMode requested) {
    // SIX 同时具有 S 与 IX 的能力；X 满足一切
    if (held == LockMode::EXCLUSIVE || held == LockMode::S_IX) {
        return true;
    }
    if (held == LockMode::SHARED) {
        return requested == LockMode::SHARED || requested == LockMode::INTENTION_SHARED;
    }
    if (held == LockMode::INTENTION_EXCLUSIVE) {
        return requested == LockMode::INTENTION_EXCLUSIVE || requested == LockMode::INTENTION_SHARED;
    }
    // INTENTION_SHARED
    return requested == LockMode::INTENTION_SHARED;
}

bool LockManager::is_compatible(LockMode requested, GroupLockMode group) {
    switch (requested) {
        case LockMode::SHARED:
            return group == GroupLockMode::NON_LOCK || group == GroupLockMode::IS || group == GroupLockMode::S;
        case LockMode::EXCLUSIVE:
            return group == GroupLockMode::NON_LOCK;
        case LockMode::INTENTION_SHARED:
            return group != GroupLockMode::X;
        case LockMode::INTENTION_EXCLUSIVE:
            return group == GroupLockMode::NON_LOCK || group == GroupLockMode::IS || group == GroupLockMode::IX;
        case LockMode::S_IX:
            return group == GroupLockMode::NON_LOCK || group == GroupLockMode::IS;
    }
    return false;
}

LockManager::GroupLockMode LockManager::combine(GroupLockMode current, LockMode granted) {
    switch (granted) {
        case LockMode::SHARED:
            if (current == GroupLockMode::IX || current == GroupLockMode::SIX) {
                return GroupLockMode::SIX;
            }
            return GroupLockMode::S;
        case LockMode::EXCLUSIVE:
            return GroupLockMode::X;
        case LockMode::INTENTION_SHARED:
            // IS 不会削弱也不会增强已有模式（IX/S/SIX/X 均蕴含 IS）
            return current == GroupLockMode::NON_LOCK ? GroupLockMode::IS : current;
        case LockMode::INTENTION_EXCLUSIVE:
            if (current == GroupLockMode::S || current == GroupLockMode::SIX) {
                return GroupLockMode::SIX;
            }
            return GroupLockMode::IX;
        case LockMode::S_IX:
            return GroupLockMode::SIX;
    }
    return current;
}

LockManager::GroupLockMode LockManager::recompute(const LockRequestQueue& queue) {
    GroupLockMode mode = GroupLockMode::NON_LOCK;
    for (const auto& request : queue.request_queue_) {
        mode = combine(mode, request.lock_mode_);
    }
    return mode;
}

bool LockManager::lock_common(Transaction* txn, LockDataId lock_data_id, LockMode lock_mode) {
    std::unique_lock<std::mutex> lock(latch_);

    if (txn->get_state() == TransactionState::ABORTED || txn->get_state() == TransactionState::COMMITTED) {
        return false;
    }
    if (txn->get_state() == TransactionState::SHRINKING) {
        // 两阶段封锁：收缩阶段不允许再申请新锁
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    auto& queue = lock_table_[lock_data_id];

    // 本事务已持有该对象的锁：已满足则直接返回，否则尝试升级
    auto it = std::find_if(queue.request_queue_.begin(), queue.request_queue_.end(),
                           [&](const LockRequest& request) { return request.txn_id_ == txn->get_transaction_id(); });
    if (it != queue.request_queue_.end()) {
        if (holds_sufficient(it->lock_mode_, lock_mode)) {
            return true;
        }
        // 升级采用 no-wait：只有本事务是该对象的唯一持锁者时才能升级成功
        if (queue.request_queue_.size() != 1 || mode_strength(lock_mode) <= mode_strength(it->lock_mode_)) {
            return false;
        }
        it->lock_mode_ = lock_mode;
        queue.group_lock_mode_ = recompute(queue);
        return true;
    }

    // no-wait：与现有组模式冲突则直接失败，由上层抛 TransactionAbortException
    if (!is_compatible(lock_mode, queue.group_lock_mode_)) {
        return false;
    }

    queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
    queue.request_queue_.back().granted_ = true;
    queue.group_lock_mode_ = combine(queue.group_lock_mode_, lock_mode);
    txn->lock_set().insert(lock_data_id);
    return true;
}

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock_common(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock_common(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXCLUSIVE);
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock_common(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock_common(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXCLUSIVE);
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return lock_common(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return lock_common(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    std::unique_lock<std::mutex> lock(latch_);

    auto queue_it = lock_table_.find(lock_data_id);
    if (queue_it == lock_table_.end()) {
        return false;
    }
    auto& queue = queue_it->second;
    auto it = std::find_if(queue.request_queue_.begin(), queue.request_queue_.end(),
                           [&](const LockRequest& request) { return request.txn_id_ == txn->get_transaction_id(); });
    if (it == queue.request_queue_.end()) {
        return false;
    }

    queue.request_queue_.erase(it);
    txn->lock_set().erase(lock_data_id);

    if (queue.request_queue_.empty()) {
        lock_table_.erase(queue_it);
    } else {
        queue.group_lock_mode_ = recompute(queue);
    }
    return true;
}
