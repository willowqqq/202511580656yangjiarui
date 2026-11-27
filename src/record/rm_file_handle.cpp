// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "rm_file_handle.h"

#include <cstring>

#include "common/context.h"
#include "transaction/concurrency/lock_manager.h"
#include "transaction/transaction.h"

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    // 1. 获取指定记录所在的page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 2. 检查该slot是否存放了记录，若没有则抛异常
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    // 3. Lab4：给该记录加上行级读锁（no-wait，失败则回滚本事务）
    if (context != nullptr && context->transaction() != nullptr) {
        Transaction* txn = context->transaction();
        if (!context->lock_manager()->lock_shared_on_record(txn, rid, fd_)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }
    // 4. 把位于指定slot的record拷贝一份返回
    auto record = std::make_unique<RmRecord>(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
    return record;
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {const char*} buf 要插入的记录数据，只读取 record_size 字节
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(const char* buf, Context* context) {
    // 0. Lab4：插入可能改变表内容（幻读风险），需要表级意向写锁
    if (context != nullptr && context->transaction() != nullptr) {
        Transaction* txn = context->transaction();
        if (!context->lock_manager()->lock_IX_on_table(txn, fd_)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }
    // 1. 获取当前未满的page handle
    RmPageHandle page_handle = create_page_handle();
    // 2. 在page handle中找到空闲slot位置
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    if (slot_no >= file_hdr_.num_records_per_page) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw InternalError("no free slot in record page");
    }
    // 3. 将buf复制到空闲slot位置
    memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
    // 4. 更新page_handle.page_hdr和bitmap
    Bitmap::set(page_handle.bitmap, slot_no);
    page_handle.page_hdr->num_records++;

    Rid rid{.page_no = page_handle.page->get_page_id().page_no, .slot_no = slot_no};
    // 5. 插入后页面已满，将该页从空闲页链表中移除
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }
    // 6. Lab4：记录写操作，用于事务回滚
    if (context != nullptr && context->transaction() != nullptr) {
        context->transaction()->append_write_record(
            WriteRecord(WType::INSERT_TUPLE, disk_manager_->get_file_name(fd_), rid));
    }
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    return rid;
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {const char*} buf 要插入的记录数据，只读取 record_size 字节
 */
void RmFileHandle::insert_record(const Rid& rid, const char* buf) {
    // Lab4 回滚扩展接口：把旧记录恢复到原来的位置
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records++;

    // 页面因恢复而变满时，需要从空闲页链表中摘除该页
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        const int page_no = page_handle.page->get_page_id().page_no;
        // 在链表中找到该页的前驱并摘除
        int prev = RM_NO_PAGE;
        int cur = file_hdr_.first_free_page_no;
        while (cur != RM_NO_PAGE && cur != page_no) {
            RmPageHandle cur_handle = fetch_page_handle(cur);
            prev = cur;
            cur = cur_handle.page_hdr->next_free_page_no;
            buffer_pool_manager_->unpin_page(cur_handle.page->get_page_id(), false);
        }
        if (cur == page_no) {
            if (prev == RM_NO_PAGE) {
                file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
            } else {
                RmPageHandle prev_handle = fetch_page_handle(prev);
                prev_handle.page_hdr->next_free_page_no = page_handle.page_hdr->next_free_page_no;
                buffer_pool_manager_->unpin_page(prev_handle.page->get_page_id(), true);
            }
        }
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    // 1. Lab4：行级写锁 + 写集记录（快照旧值，供回滚恢复）
    if (context != nullptr && context->transaction() != nullptr) {
        Transaction* txn = context->transaction();
        if (!context->lock_manager()->lock_exclusive_on_record(txn, rid, fd_)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
        {
            RmPageHandle snapshot_handle = fetch_page_handle(rid.page_no);
            RmRecord snapshot(file_hdr_.record_size, snapshot_handle.get_slot(rid.slot_no));
            buffer_pool_manager_->unpin_page(snapshot_handle.page->get_page_id(), false);
            txn->append_write_record(
                WriteRecord(WType::DELETE_TUPLE, disk_manager_->get_file_name(fd_), rid, std::move(snapshot)));
        }
    }
    // 2. 获取指定记录所在的page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 3. 将bitmap中对应槽位置0，并减少记录数
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;
    // 4. 若该页从已满变为未满，将其重新加入空闲页链表
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page - 1) {
        release_page_handle(page_handle);
    }
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {const char*} buf 新记录的数据，只读取 record_size 字节
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, const char* buf, Context* context) {
    // 1. Lab4：行级写锁 + 写集记录（快照旧值，供回滚恢复）
    if (context != nullptr && context->transaction() != nullptr) {
        Transaction* txn = context->transaction();
        if (!context->lock_manager()->lock_exclusive_on_record(txn, rid, fd_)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
        {
            RmPageHandle snapshot_handle = fetch_page_handle(rid.page_no);
            RmRecord snapshot(file_hdr_.record_size, snapshot_handle.get_slot(rid.slot_no));
            buffer_pool_manager_->unpin_page(snapshot_handle.page->get_page_id(), false);
            txn->append_write_record(
                WriteRecord(WType::UPDATE_TUPLE, disk_manager_->get_file_name(fd_), rid, std::move(snapshot)));
        }
    }
    // 2. 获取指定记录所在的page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 3. 更新记录
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

/**
 * 以下函数为辅助函数，仅提供参考，可以选择完成如下函数，也可以删除如下函数，在单元测试中不涉及如下函数接口的直接调用
 */
/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    // 使用缓冲池获取指定页面，并生成page_handle返回给上层
    // if page_no is invalid, throw PageNotExistError exception
    if (page_no < RM_FIRST_RECORD_PAGE || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError(disk_manager_->get_file_name(fd_), page_no);
    }
    Page* page = buffer_pool_manager_->fetch_page(PageId{.fd = fd_, .page_no = page_no});
    if (page == nullptr) {
        throw InternalError("failed to fetch record page");
    }
    return {&file_hdr_, page};
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    // 1.使用缓冲池来创建一个新page
    PageId page_id{.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page* page = buffer_pool_manager_->new_page(&page_id);
    if (page == nullptr) {
        throw InternalError("failed to create a new record page");
    }
    // 2.更新page handle中的相关信息
    RmPageHandle page_handle(&file_hdr_, page);
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    page_handle.page_hdr->num_records = 0;
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);
    // 3.更新file_hdr_：新页加入空闲页链表头，页面数加1
    file_hdr_.first_free_page_no = page_id.page_no;
    file_hdr_.num_pages++;
    return page_handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle() {
    // 1. 判断file_hdr_中是否还有空闲页
    //     1.1 没有空闲页：使用缓冲池来创建一个新page；可直接调用create_new_page_handle()
    //     1.2 有空闲页：直接获取第一个空闲页
    // 2. 生成page handle并返回给上层
    if (file_hdr_.first_free_page_no != RM_NO_PAGE) {
        return fetch_page_handle(file_hdr_.first_free_page_no);
    }
    return create_new_page_handle();
}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle& page_handle) {
    // 当page从已满变成未满，考虑如何更新：
    // 1. page_handle.page_hdr->next_free_page_no
    // 2. file_hdr_.first_free_page_no
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
}