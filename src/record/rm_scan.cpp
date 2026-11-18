// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "rm_scan.h"

#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle* file_handle) : file_handle_(file_handle) {
    // 初始化file_handle和rid（指向第一个存放了记录的位置）
    rid_ = {.page_no = RM_FIRST_RECORD_PAGE, .slot_no = -1};
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置
    while (rid_.page_no != RM_NO_PAGE && rid_.page_no < file_handle_->file_hdr_.num_pages) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(rid_.page_no);
        int slot_no =
            Bitmap::next_bit(true, page_handle.bitmap, file_handle_->file_hdr_.num_records_per_page, rid_.slot_no);
        file_handle_->buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        if (slot_no < file_handle_->file_hdr_.num_records_per_page) {
            rid_.slot_no = slot_no;
            return;
        }
        // 当前页没有更多记录，移动到下一页
        rid_.page_no++;
        rid_.slot_no = -1;
    }
    // 已遍历完所有页面，标记为末尾
    rid_ = {.page_no = RM_NO_PAGE, .slot_no = -1};
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // 到达文件末尾时 page_no 为 RM_NO_PAGE
    return rid_.page_no == RM_NO_PAGE;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const { return rid_; }