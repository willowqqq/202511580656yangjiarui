// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "b_plus_tree.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <memory>

#include "common/errors.h"
#include "index_scan.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/page.h"

int compare_index_key(const char* left, const char* right, ColType type, int column_length) {
    if (left == nullptr || right == nullptr || column_length <= 0) {
        throw InternalError("Invalid index comparison input");
    }
    switch (type) {
        case TYPE_INT: {
            if (column_length != static_cast<int>(sizeof(int))) {
                throw InternalError("Invalid integer index key length");
            }
            int left_value;
            int right_value;
            memcpy(&left_value, left, sizeof(left_value));
            memcpy(&right_value, right, sizeof(right_value));
            return (left_value < right_value) ? -1 : ((left_value > right_value) ? 1 : 0);
        }
        case TYPE_FLOAT: {
            if (column_length != static_cast<int>(sizeof(float))) {
                throw InternalError("Invalid float index key length");
            }
            float left_value;
            float right_value;
            memcpy(&left_value, left, sizeof(left_value));
            memcpy(&right_value, right, sizeof(right_value));
            return (left_value < right_value) ? -1 : ((left_value > right_value) ? 1 : 0);
        }
        case TYPE_STRING:
            return memcmp(left, right, column_length);
        default:
            throw InternalError("Unexpected data type");
    }
}

int compare_index_key(const char* left,
                      const char* right,
                      const std::vector<ColType>& column_types,
                      const std::vector<int>& column_lengths) {
    if (column_types.size() != column_lengths.size()) {
        throw InternalError("Invalid composite index metadata");
    }
    int offset = 0;
    for (size_t i = 0; i < column_types.size(); ++i) {
        int result = compare_index_key(left + offset, right + offset, column_types[i], column_lengths[i]);
        if (result != 0) return result;
        offset += column_lengths[i];
    }
    return 0;
}

BPlusTreeNode::BPlusTreeNode(const IndexFileHeader* file_header, Page* page) : file_header_(file_header), page_(page) {
    page_header_ = reinterpret_cast<IndexPageHeader*>(page_->get_data());
    keys_ = page_->get_data() + sizeof(IndexPageHeader);
    rids_ = reinterpret_cast<Rid*>(keys_ + file_header_->keys_region_size_);
}

page_id_t BPlusTreeNode::get_page_no() const noexcept { return page_->get_page_id().page_no; }

PageId BPlusTreeNode::get_page_id() const noexcept { return page_->get_page_id(); }

page_id_t BPlusTreeNode::remove_and_return_only_child() {
    assert(get_size() == 1);
    page_id_t child_page_no = value_at(0);
    erase_pair(0);
    assert(get_size() == 0);
    return child_page_no;
}

int BPlusTreeNode::find_child(const BPlusTreeNode* child) const {
    int child_index = 0;
    while (child_index < page_header_->key_count && get_rid(child_index)->page_no != child->get_page_no()) {
        ++child_index;
    }
    assert(child_index < page_header_->key_count);
    return child_index;
}

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int BPlusTreeNode::lower_bound(const char* target) const {
    int left = 0;
    int right = get_size();
    while (left < right) {
        const int mid = left + (right - left) / 2;
        if (compare_index_key(get_key(mid), target, file_header_->column_types_, file_header_->column_lengths_) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[0,num_key]，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 内部结点用它定位孩子时，返回 0 表示 target 比第一个 key 还小（只会出现在最左侧路径上）。
 */
int BPlusTreeNode::upper_bound(const char* target) const {
    int left = 0;
    int right = get_size();
    while (left < right) {
        const int mid = left + (right - left) / 2;
        if (compare_index_key(get_key(mid), target, file_header_->column_types_, file_header_->column_lengths_) <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool BPlusTreeNode::leaf_lookup(const char* key, Rid** value) {
    const int pos = lower_bound(key);
    if (pos < get_size() &&
        compare_index_key(get_key(pos), key, file_header_->column_types_, file_header_->column_lengths_) == 0) {
        *value = get_rid(pos);
        return true;
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t BPlusTreeNode::internal_lookup(const char* key) {
    const int pos = upper_bound(key);
    return value_at(pos <= 0 ? 0 : pos - 1);
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param key 连续键数组的起始地址。
 * @param rid 连续 Rid 数组的起始地址。
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void BPlusTreeNode::insert_pairs(int pos, const char* key, const Rid* rid, int n) {
    assert(pos >= 0 && pos <= get_size());
    const int size = get_size();
    const int key_length = file_header_->key_length_;

    if (size - pos > 0) {
        memmove(get_key(pos + n), get_key(pos), static_cast<size_t>(size - pos) * key_length);
        memmove(get_rid(pos + n), get_rid(pos), static_cast<size_t>(size - pos) * sizeof(Rid));
    }
    memcpy(get_key(pos), key, static_cast<size_t>(n) * key_length);
    memcpy(get_rid(pos), rid, static_cast<size_t>(n) * sizeof(Rid));
    set_size(size + n);
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param key 要插入的键。
 * @param value 要插入的 Rid。
 * @return int 键值对数量
 */
int BPlusTreeNode::insert(const char* key, const Rid& value) {
    const int pos = upper_bound(key);
    insert_pairs(pos, key, &value, 1);
    return get_size();
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void BPlusTreeNode::erase_pair(int pos) {
    assert(pos >= 0 && pos < get_size());
    const int size = get_size();
    const int key_length = file_header_->key_length_;
    if (size - pos - 1 > 0) {
        memmove(get_key(pos), get_key(pos + 1), static_cast<size_t>(size - pos - 1) * key_length);
        memmove(get_rid(pos), get_rid(pos + 1), static_cast<size_t>(size - pos - 1) * sizeof(Rid));
    }
    set_size(size - 1);
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int BPlusTreeNode::remove(const char* key, const Rid& rid) {
    // 同一 key 可能有多条，必须同时匹配 rid，只删第一条匹配的
    int pos = lower_bound(key);
    const int size = get_size();
    while (pos < size &&
           compare_index_key(get_key(pos), key, file_header_->column_types_, file_header_->column_lengths_) == 0) {
        const Rid* candidate = get_rid(pos);
        if (candidate->page_no == rid.page_no && candidate->slot_no == rid.slot_no) {
            erase_pair(pos);
            break;
        }
        ++pos;
    }
    return get_size();
}

BPlusTree::BPlusTree(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager, int file_descriptor)
    : disk_manager_(disk_manager),
      buffer_pool_manager_(buffer_pool_manager),
      file_descriptor_(file_descriptor),
      file_header_() {
    alignas(int) char buf[PAGE_SIZE]{};
    disk_manager_->read_page(file_descriptor_, INDEX_FILE_HEADER_PAGE, buf, PAGE_SIZE);
    auto file_header = std::make_unique<IndexFileHeader>();
    file_header->deserialize(buf);

    // page_count_ is not a physical high-water mark after deletion or a crash.
    // Never reuse a page that is already present in the file.
    int64_t file_size = disk_manager_->get_file_size(file_descriptor_);
    if (file_size < 0) {
        throw InternalError("BPlusTree: cannot stat index file");
    }
    int64_t physical_pages_64 = file_size / PAGE_SIZE + (file_size % PAGE_SIZE != 0);
    if (physical_pages_64 > std::numeric_limits<page_id_t>::max()) {
        throw InternalError("BPlusTree: index file is too large");
    }
    auto physical_pages = static_cast<page_id_t>(physical_pages_64);
    disk_manager_->set_fd2pageno(file_descriptor_, std::max(file_header->page_count_, physical_pages));
    file_header_ = std::move(file_header);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @param find_first 是否从叶子链表的最左端开始查找
 * @return 目标叶子结点（IndexNode 离开作用域即 unpin）以及根结点是否加锁
 */
std::pair<IndexNode, bool> BPlusTree::find_leaf_page(const char* key,
                                                     IndexOperation operation,
                                                     Transaction* transaction,
                                                     bool find_first) {
    if (is_empty()) {
        return {IndexNode{}, false};
    }

    if (find_first) {
        return {fetch_node(file_header_->first_leaf_), false};
    }

    IndexNode node = fetch_node(file_header_->root_page_);
    if (!node.valid()) {
        return {IndexNode{}, false};
    }

    while (!node->is_leaf_page()) {
        const page_id_t child_page = node->internal_lookup(key);
        IndexNode child = fetch_node(child_page);
        if (!child.valid()) {
            return {IndexNode{}, false};
        }
        node = std::move(child);
    }

    return {std::move(node), false};
}

/**
 * @brief 查找指定 key 对应的全部 Rid。
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 * @note 非唯一索引中，同一 key 的记录可能因分裂分布在多个相邻叶子里。
 */
bool BPlusTree::get_value(const char* key, std::vector<Rid>* result, Transaction* transaction) {
    std::lock_guard<std::recursive_mutex> latch(root_latch_);
    result->clear();

    IndexNode leaf = find_leaf_page(key, IndexOperation::Find, transaction, false).first;
    if (!leaf.valid()) {
        return false;
    }

    while (leaf->get_prev_leaf() != INDEX_LEAF_HEADER_PAGE) {
        IndexNode prev = fetch_node(leaf->get_prev_leaf());
        if (!prev.valid() || prev->get_size() == 0 ||
            compare_index_key(prev->get_key(prev->get_size() - 1), key, file_header_->column_types_,
                              file_header_->column_lengths_) < 0) {
            break;
        }
        leaf = std::move(prev);
    }

    bool found = false;
    while (leaf.valid()) {
        if (leaf->get_size() == 0 ||
            compare_index_key(leaf->get_key(0), key, file_header_->column_types_, file_header_->column_lengths_) > 0) {
            break;
        }

        int pos = leaf->lower_bound(key);
        const int size = leaf->get_size();
        while (pos < size &&
               compare_index_key(leaf->get_key(pos), key, file_header_->column_types_, file_header_->column_lengths_) ==
                   0) {
            result->push_back(*leaf->get_rid(pos));
            found = true;
            ++pos;
        }

        const page_id_t next = leaf->get_next_leaf();
        if (next == INDEX_LEAF_HEADER_PAGE) {
            break;
        }
        leaf = fetch_node(next);
    }

    return found;
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note 返回的 IndexNode 拥有新页的 pin；原 node 仍由调用方持有。
 */
IndexNode BPlusTree::split(IndexNode& node) {
    const int size = node->get_size();
    const int split_pos = size / 2;
    const int right_count = size - split_pos;
    const bool is_leaf = node->is_leaf_page();

    IndexNode new_node = create_node();
    if (!new_node.valid()) {
        throw InternalError("split: failed to create new node");
    }

    new_node->set_parent_page_no(node->get_parent_page_no());
    new_node->set_size(0);
    new_node->set_prev_leaf(INDEX_NO_PAGE);
    new_node->set_next_leaf(INDEX_NO_PAGE);
    new_node->page_header_->is_leaf = is_leaf;

    // 右半部分键值对移动到新结点
    memcpy(new_node->get_key(0), node->get_key(split_pos), static_cast<size_t>(right_count) * file_header_->key_length_);
    memcpy(new_node->get_rid(0), node->get_rid(split_pos), static_cast<size_t>(right_count) * sizeof(Rid));
    new_node->set_size(right_count);
    node->set_size(split_pos);

    if (is_leaf) {
        const page_id_t old_next = node->get_next_leaf();
        new_node->set_prev_leaf(node->get_page_no());
        new_node->set_next_leaf(old_next);
        node->set_next_leaf(new_node->get_page_no());

        if (old_next == INDEX_LEAF_HEADER_PAGE) {
            IndexNode sentinel = fetch_node(INDEX_LEAF_HEADER_PAGE);
            sentinel->set_prev_leaf(new_node->get_page_no());
            sentinel.mark_dirty();
        } else {
            IndexNode next_leaf = fetch_node(old_next);
            next_leaf->set_prev_leaf(new_node->get_page_no());
            next_leaf.mark_dirty();
        }
    } else {
        for (int i = 0; i < right_count; ++i) {
            update_child_parent(new_node, i);
        }
    }

    node.mark_dirty();
    new_node.mark_dirty();
    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param old_node 分裂后保留左半部分的原结点。
 * @param key 要插入parent的key
 * @param new_node 分裂产生的右兄弟结点。
 * @param transaction 当前事务；不需要并发控制时可为 nullptr。
 * @note 一个结点插入了键值对之后需要分裂，分裂后左半部分的键值对保留在原结点，在参数中称为old_node，
 * 右半部分的键值对分裂为新的右兄弟节点，在参数中称为new_node（参考Split函数来理解old_node和new_node）
 * @note old_node / new_node 的 pin 仍由调用方的 IndexNode 持有。
 */
void BPlusTree::insert_into_parent(IndexNode& old_node,
                                   const char* key,
                                   IndexNode& new_node,
                                   Transaction* transaction) {
    if (old_node->is_root_page()) {
        IndexNode new_root = create_node();
        if (!new_root.valid()) {
            throw InternalError("insert_into_parent: failed to create new root");
        }
        new_root->set_parent_page_no(INDEX_NO_PAGE);
        new_root->set_size(0);
        new_root->set_prev_leaf(INDEX_NO_PAGE);
        new_root->set_next_leaf(INDEX_NO_PAGE);
        new_root->page_header_->is_leaf = false;

        new_root->set_key(0, old_node->get_key(0));
        new_root->set_rid(0, {.page_no = old_node->get_page_no(), .slot_no = -1});
        new_root->set_key(1, key);
        new_root->set_rid(1, {.page_no = new_node->get_page_no(), .slot_no = -1});
        new_root->set_size(2);

        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());

        old_node.mark_dirty();
        new_node.mark_dirty();
        new_root.mark_dirty();

        file_header_->root_page_ = new_root->get_page_no();
        return;
    }

    IndexNode parent = fetch_node(old_node->get_parent_page_no());
    if (!parent.valid()) {
        throw InternalError("insert_into_parent: failed to fetch parent");
    }

    const int rank = parent->find_child(&old_node.node());
    const Rid new_rid = {.page_no = new_node->get_page_no(), .slot_no = -1};
    parent->insert_pairs(rank + 1, key, &new_rid, 1);
    parent.mark_dirty();

    if (parent->get_size() == parent->get_max_size()) {
        IndexNode new_parent = split(parent);
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
    }
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param key 要插入的键。
 * @param value 键对应的 Rid。
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 * @throws DuplicateKeyError 唯一索引中已存在相同 key，此时树保持不变。
 * @pre 调用方不会重复插入完全相同的 (key, value)。
 */
page_id_t BPlusTree::insert_entry(const char* key, const Rid& value, Transaction* transaction) {
    std::lock_guard<std::recursive_mutex> latch(root_latch_);
    if (is_unique()) {
        std::vector<Rid> existing;
        if (get_value(key, &existing, transaction)) {
            throw DuplicateKeyError();
        }
    }

    if (is_empty()) {
        IndexNode new_root = create_node();
        if (!new_root.valid()) {
            throw InternalError("insert_entry: failed to create root leaf");
        }
        new_root->set_parent_page_no(INDEX_NO_PAGE);
        new_root->set_size(0);
        new_root->set_prev_leaf(INDEX_LEAF_HEADER_PAGE);
        new_root->set_next_leaf(INDEX_LEAF_HEADER_PAGE);
        new_root->page_header_->is_leaf = true;

        IndexNode sentinel = fetch_node(INDEX_LEAF_HEADER_PAGE);
        sentinel->set_prev_leaf(new_root->get_page_no());
        sentinel->set_next_leaf(new_root->get_page_no());
        sentinel.mark_dirty();

        file_header_->root_page_ = new_root->get_page_no();
        file_header_->first_leaf_ = new_root->get_page_no();
        file_header_->last_leaf_ = new_root->get_page_no();

        new_root->insert(key, value);
        new_root.mark_dirty();
        return new_root->get_page_no();
    }

    IndexNode leaf = find_leaf_page(key, IndexOperation::Insert, transaction, false).first;
    if (!leaf.valid()) {
        throw InternalError("insert_entry: failed to find leaf page");
    }

    const page_id_t leaf_page_no = leaf->get_page_no();
    leaf->insert(key, value);
    leaf.mark_dirty();

    if (leaf->get_size() == leaf->get_max_size()) {
        IndexNode new_leaf = split(leaf);
        if (leaf_page_no == file_header_->last_leaf_) {
            file_header_->last_leaf_ = new_leaf->get_page_no();
        }
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);
    }

    // 若插入的是该叶子的新最小 key，需要更新祖先结点中的 key（保持"父结点 key = 子树最小值"）
    update_ancestor_keys(leaf);

    return leaf_page_no;
}

/**
 * @brief 删除 B+ 树中的一条 (key, rid)。
 * @param key 要删除的key值
 * @param rid 要删除的记录位置；同一 key 的其他记录保留
 * @param transaction 事务指针
 * @return 该 (key, rid) 存在并被删除时返回 true
 */
bool BPlusTree::delete_entry(const char* key, const Rid& rid, Transaction* transaction) {
    std::lock_guard<std::recursive_mutex> latch(root_latch_);

    IndexNode leaf = find_leaf_page(key, IndexOperation::Delete, transaction, false).first;
    if (!leaf.valid()) {
        return false;
    }

    // 同一 key 的记录可能分布在左侧相邻叶子中（get_value 同理）
    while (leaf->get_prev_leaf() != INDEX_LEAF_HEADER_PAGE) {
        IndexNode prev = fetch_node(leaf->get_prev_leaf());
        if (!prev.valid() || prev->get_size() == 0 ||
            compare_index_key(prev->get_key(prev->get_size() - 1), key, file_header_->column_types_,
                              file_header_->column_lengths_) < 0) {
            break;
        }
        leaf = std::move(prev);
    }

    // 向右逐叶查找并删除唯一一条 (key, rid)
    bool deleted = false;
    while (leaf.valid()) {
        if (leaf->get_size() == 0 ||
            compare_index_key(leaf->get_key(0), key, file_header_->column_types_, file_header_->column_lengths_) > 0) {
            break;
        }
        const int size_before = leaf->get_size();
        leaf->remove(key, rid);
        if (leaf->get_size() != size_before) {
            deleted = true;
            leaf.mark_dirty();
            break;
        }
        const page_id_t next = leaf->get_next_leaf();
        if (next == INDEX_LEAF_HEADER_PAGE) {
            break;
        }
        leaf = fetch_node(next);
    }

    if (!deleted) {
        return false;
    }

    // 若删除的是叶子里的第一个 key，祖先结点中对应的 key 需要更新
    if (leaf->get_size() > 0) {
        update_ancestor_keys(leaf);
    }

    // 下溢处理：根结点特殊处理，否则合并或重分配
    if (leaf->get_size() < leaf->get_min_size()) {
        coalesce_or_redistribute(leaf, transaction, nullptr);
    }
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 * @note User needs to first find the sibling of input page.
 * If sibling's size + input page's size >= 2 * page's minsize, then redistribute.
 * Otherwise, merge(Coalesce).
 */
bool BPlusTree::coalesce_or_redistribute(IndexNode& node, Transaction* transaction, bool* root_is_latched) {
    // 根结点可以低于半满，只需做根的特殊调整
    if (node->is_root_page()) {
        return adjust_root(node);
    }
    if (node->get_size() >= node->get_min_size()) {
        return false;
    }

    IndexNode parent = fetch_node(node->get_parent_page_no());
    if (!parent.valid()) {
        throw InternalError("coalesce_or_redistribute: failed to fetch parent");
    }
    const int index = parent->find_child(&node.node());

    // 优先选取前驱结点作为兄弟，否则取后继
    IndexNode neighbor;
    if (index > 0) {
        neighbor = fetch_node(parent->value_at(index - 1));
    } else {
        neighbor = fetch_node(parent->value_at(index + 1));
    }
    if (!neighbor.valid()) {
        throw InternalError("coalesce_or_redistribute: failed to fetch sibling");
    }

    // 两个结点的键值对数量之和足以支撑两个结点 → 重分配；否则合并
    if (node->get_size() + neighbor->get_size() >= 2 * node->get_min_size()) {
        redistribute(neighbor, node, parent, index);
        return false;
    }
    return coalesce(neighbor, node, parent, index, transaction, root_is_latched);
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 * @note size of root page can be less than min size and this method is only called within coalesce_or_redistribute()
 */
bool BPlusTree::adjust_root(IndexNode& old_root_node) {
    // 内部根结点只剩一个孩子 → 孩子成为新根
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        const page_id_t child_page = old_root_node->value_at(0);
        IndexNode child = fetch_node(child_page);
        if (!child.valid()) {
            throw InternalError("adjust_root: failed to fetch child of collapsed root");
        }
        child->set_parent_page_no(INDEX_NO_PAGE);
        child.mark_dirty();

        const page_id_t old_root_page = old_root_node->get_page_no();
        file_header_->root_page_ = child_page;
        {
            auto dead_guard = old_root_node.release();  // 交出 pin，否则 delete_page 会失败
            (void)dead_guard;
        }
        buffer_pool_manager_->delete_page(PageId{.fd = file_descriptor_, .page_no = old_root_page});
        record_page_deletion();
        return true;
    }
    // 叶根结点被删空 → 整棵树为空
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        unlink_leaf(old_root_node);  // 哨兵页 prev/next 指回自己
        const page_id_t old_root_page = old_root_node->get_page_no();
        file_header_->root_page_ = INDEX_NO_PAGE;
        file_header_->first_leaf_ = INDEX_LEAF_HEADER_PAGE;
        file_header_->last_leaf_ = INDEX_LEAF_HEADER_PAGE;
        {
            auto dead_guard = old_root_node.release();
            (void)dead_guard;
        }
        buffer_pool_manager_->delete_page(PageId{.fd = file_descriptor_, .page_no = old_root_page});
        record_page_deletion();
        return true;
    }
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 * Redistribute key & value pairs from one page to its sibling page. If index == 0, move sibling page's first key
 * & value pair into end of input "node", otherwise move sibling page's last key & value pair into head of input "node".
 *
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 * @note node是之前刚被删除过一个key的结点
 * index=0，则neighbor是node后继结点，表示：node(left)      neighbor(right)
 * index>0，则neighbor是node前驱结点，表示：neighbor(left)  node(right)
 * 注意更新parent结点的相关kv对
 */
void BPlusTree::redistribute(IndexNode& neighbor_node, IndexNode& node, IndexNode& parent, int index) {
    (void)parent;  // 父结点 key 的更新统一由 update_ancestor_keys 完成
    if (index == 0) {
        // neighbor 是 node 的后继：把 neighbor 的第一个键值对移到 node 的末尾
        node->insert_pairs(node->get_size(), neighbor_node->get_key(0), neighbor_node->get_rid(0), 1);
        neighbor_node->erase_pair(0);
        if (!node->is_leaf_page()) {
            update_child_parent(node, node->get_size() - 1);
        }
        // neighbor 的第一个 key 变了，需要更新父结点（及可能更高的祖先）
        update_ancestor_keys(neighbor_node);
    } else {
        // neighbor 是 node 的前驱：把 neighbor 的最后一个键值对移到 node 的开头
        const int last = neighbor_node->get_size() - 1;
        node->insert_pairs(0, neighbor_node->get_key(last), neighbor_node->get_rid(last), 1);
        neighbor_node->erase_pair(last);
        if (!node->is_leaf_page()) {
            update_child_parent(node, 0);
        }
        // node 的第一个 key 变了，需要更新父结点（及可能更高的祖先）
        update_ancestor_keys(node);
    }
    node.mark_dirty();
    neighbor_node.mark_dirty();
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并，也就是和它左边的neighbor_node进行合并；
 * 假设node一定在右边。如果上层传入的index=0，说明node在左边，那么交换node和neighbor_node，保证node在右边；合并到左结点，实际上就是删除了右结点；
 * Move all the key & value pairs from one page to its sibling page, and notify buffer pool manager to delete this page.
 * Parent page must be adjusted to take info of deletion into account. Remember to deal with coalesce or redistribute
 * recursively if necessary.
 *
 * @param neighbor_node sibling page of input "node" (neighbor_node是node的前结点)
 * @param node input from method coalesceOrRedistribute() (node结点是需要被删除的)
 * @param parent parent page of input "node"
 * @param index node在parent中的rid_idx
 * @param transaction 当前事务；不需要并发控制时可为 nullptr。
 * @param root_is_latched 传出根结点的加锁状态。
 * @return true means parent node should be deleted, false means no deletion happend
 * @note Assume that *neighbor_node is the left sibling of *node (neighbor -> node)
 */
bool BPlusTree::coalesce(IndexNode& neighbor_node,
                         IndexNode& node,
                         IndexNode& parent,
                         int index,
                         Transaction* transaction,
                         bool* root_is_latched) {
    // 约定 node 在右、neighbor 在左；index == 0 说明 node 在最左，交换后合并方向不变
    if (index == 0) {
        std::swap(neighbor_node, node);
        index = 1;
    }

    // 1. 把 node 的全部键值对追加到 neighbor 末尾
    const int neighbor_size = neighbor_node->get_size();
    const int node_size = node->get_size();
    neighbor_node->insert_pairs(neighbor_size, node->get_key(0), node->get_rid(0), node_size);
    if (!node->is_leaf_page()) {
        for (int i = 0; i < node_size; ++i) {
            update_child_parent(neighbor_node, neighbor_size + i);
        }
    }

    // 2. 叶子链表维护：若删除的是最后一个叶子，更新 last_leaf_
    if (node->is_leaf_page()) {
        const bool is_last_leaf = node->get_next_leaf() == INDEX_LEAF_HEADER_PAGE;
        unlink_leaf(node);
        if (is_last_leaf) {
            file_header_->last_leaf_ = neighbor_node->get_page_no();
        }
    }

    // 3. 从 parent 中删除 node 的键值对
    parent->erase_pair(index);
    parent.mark_dirty();
    neighbor_node.mark_dirty();

    // 4. 删除 node 页面：先交出 pin，否则 delete_page 因页面仍被固定而失败
    const page_id_t dead_page = node->get_page_no();
    {
        auto dead_guard = node.release();
        (void)dead_guard;
    }
    buffer_pool_manager_->delete_page(PageId{.fd = file_descriptor_, .page_no = dead_page});
    record_page_deletion();

    // 5. parent 下溢则继续递归处理；根结点交给 adjust_root
    if (parent->is_root_page()) {
        return adjust_root(parent);
    }
    if (parent->get_size() < parent->get_min_size()) {
        return coalesce_or_redistribute(parent, transaction, root_is_latched);
    }
    return true;
}

/**
 * @brief 将索引扫描位置转换为表记录位置 Rid。
 *
 * @param position 叶子页号和页内槽号。
 * @return 对应索引项保存的 Rid。
 * @note IndexPosition 描述索引内部位置，Rid 描述表记录位置，二者含义不同。
 */
Rid BPlusTree::get_rid(const IndexPosition& position) const {
    IndexNode node = fetch_node(position.page_no);
    if (!node.valid() || position.slot_no < 0 || position.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    return *node->get_rid(position.slot_no);
}

/**
 * @brief 返回整棵树中第一个 key >= 目标 key 的索引项位置。
 *
 * @param key 目标 key
 * @return IndexPosition 可作为 IndexScan 的起点
 * @note 位置规范：若结果恰好落在某个非最后叶子的末尾（slot_no == size），应改为下一叶子的
 *       第 0 个槽；若所有 key 都小于目标 key，返回 leaf_end()。这样得到的位置才能与
 *       IndexScan::next() 产生的位置直接比较。
 */
IndexPosition BPlusTree::lower_bound(const char* key) {
    std::lock_guard<std::recursive_mutex> latch(root_latch_);
    IndexNode leaf = find_leaf_page(key, IndexOperation::Find, nullptr, false).first;
    if (!leaf.valid()) {
        return leaf_end();
    }

    while (leaf->get_prev_leaf() != INDEX_LEAF_HEADER_PAGE) {
        IndexNode prev = fetch_node(leaf->get_prev_leaf());
        if (!prev.valid() || prev->get_size() == 0 ||
            compare_index_key(prev->get_key(prev->get_size() - 1), key, file_header_->column_types_,
                              file_header_->column_lengths_) < 0) {
            break;
        }
        leaf = std::move(prev);
    }

    const int slot = leaf->lower_bound(key);
    if (slot == leaf->get_size() && leaf->get_page_no() != file_header_->last_leaf_) {
        return {.page_no = leaf->get_next_leaf(), .slot_no = 0};
    }
    return {.page_no = leaf->get_page_no(), .slot_no = slot};
}

/**
 * @brief 返回整棵树中第一个 key > 目标 key 的索引项位置。
 *
 * @param key 目标 key
 * @return IndexPosition 可作为 IndexScan 的终点
 * @note 位置规范与 lower_bound() 相同。
 */
IndexPosition BPlusTree::upper_bound(const char* key) {
    std::lock_guard<std::recursive_mutex> latch(root_latch_);
    IndexNode leaf = find_leaf_page(key, IndexOperation::Find, nullptr, false).first;
    if (!leaf.valid()) {
        return leaf_end();
    }

    const int slot = leaf->upper_bound(key);
    if (slot == leaf->get_size() && leaf->get_page_no() != file_header_->last_leaf_) {
        return {.page_no = leaf->get_next_leaf(), .slot_no = 0};
    }
    return {.page_no = leaf->get_page_no(), .slot_no = slot};
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 * 用处在于可以作为IndexScan的最后一个
 *
 * @return IndexPosition
 */
IndexPosition BPlusTree::leaf_end() const {
    IndexNode node = fetch_node(file_header_->last_leaf_);
    if (!node.valid()) {
        throw InternalError("failed to fetch last leaf for index scan");
    }
    return {.page_no = file_header_->last_leaf_, .slot_no = node->get_size()};
}

/**
 * @brief 指向第一个叶子的第一个结点
 * 用处在于可以作为IndexScan的第一个
 *
 * @return IndexPosition
 */
IndexPosition BPlusTree::leaf_begin() const { return {.page_no = file_header_->first_leaf_, .slot_no = 0}; }

IndexNode BPlusTree::fetch_node(page_id_t page_no) const {
    PageGuard guard = buffer_pool_manager_->fetch_page_guard(PageId{.fd = file_descriptor_, .page_no = page_no});
    return {file_header_.get(), std::move(guard)};
}

IndexNode BPlusTree::create_node() {
    PageId new_page_id = {.fd = file_descriptor_, .page_no = INVALID_PAGE_ID};
    PageGuard guard = buffer_pool_manager_->new_page_guard(&new_page_id);
    if (!guard) {
        return {};
    }
    file_header_->page_count_++;
    return {file_header_.get(), std::move(guard)};
}

void BPlusTree::update_ancestor_keys(IndexNode& node) {
    page_id_t current_page = node->get_page_no();
    while (true) {
        IndexNode current = fetch_node(current_page);
        if (!current.valid() || current->get_parent_page_no() == INDEX_NO_PAGE) {
            break;
        }
        IndexNode parent = fetch_node(current->get_parent_page_no());
        if (!parent.valid()) {
            break;
        }
        const int rank = parent->find_child(&current.node());
        if (memcmp(parent->get_key(rank), current->get_key(0), file_header_->key_length_) == 0) {
            break;
        }
        parent->set_key(rank, current->get_key(0));
        parent.mark_dirty();
        current_page = parent->get_page_no();
    }
}

// 框架辅助：调用即可。这段失败回滚是框架内部实现，作业不要模仿。
void BPlusTree::unlink_leaf(IndexNode& leaf) {
    if (!leaf.valid() || !leaf->is_leaf_page()) {
        throw InternalError("unlink_leaf requires a valid leaf node");
    }

    const page_id_t leaf_page = leaf->get_page_no();
    const page_id_t prev_page = leaf->get_prev_leaf();
    const page_id_t next_page = leaf->get_next_leaf();

    {
        IndexNode prev = fetch_node(prev_page);
        if (!prev.valid()) {
            throw InternalError("unlink_leaf failed to fetch previous leaf");
        }
        prev->set_next_leaf(next_page);
        prev.mark_dirty();
    }
    {
        IndexNode next = fetch_node(next_page);
        if (!next.valid()) {
            IndexNode prev = fetch_node(prev_page);
            if (prev.valid()) {
                prev->set_next_leaf(leaf_page);
                prev.mark_dirty();
            }
            throw InternalError("unlink_leaf failed to fetch next leaf; previous link rolled back");
        }
        next->set_prev_leaf(prev_page);
        next.mark_dirty();
    }
}

void BPlusTree::record_page_deletion() { file_header_->page_count_--; }

void BPlusTree::update_child_parent(IndexNode& node, int child_idx) {
    if (!node.valid() || node->is_leaf_page()) {
        return;
    }
    IndexNode child = fetch_node(node->value_at(child_idx));
    if (!child.valid()) {
        return;
    }
    child->set_parent_page_no(node->get_page_no());
    child.mark_dirty();
}