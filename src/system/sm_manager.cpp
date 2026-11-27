// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "sm_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "common/context.h"
#include "common/wire_result.h"
#include "index/b_plus_tree.h"
#include "index/index_manager.h"
#include "record/rm_manager.h"
#include "record/rm_scan.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"

namespace {

namespace fs = std::filesystem;

/**
 * @brief 将数据库名约束为当前目录下的单个路径分量。
 * @throws RMDBError 名称为空、包含父目录或包含路径分隔符。
 */
fs::path database_path(const std::string& db_name) {
    fs::path path(db_name);
    if (path.empty() || path != path.filename() || path == "." || path == "..") {
        throw RMDBError("Invalid database name: " + db_name);
    }
    return path;
}

/**
 * @brief 序列化后再覆盖元数据文件，避免序列化失败时提前清空旧文件。
 * @throws InternalError 序列化、打开或写入失败。
 */
void write_meta_file(const fs::path& path, const DbMeta& db) {
    std::ostringstream serialized;
    serialized << db;
    if (!serialized) {
        throw InternalError("cannot serialize database metadata");
    }

    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw InternalError("cannot open database metadata: " + path.string());
    }
    output << serialized.str();
    output.flush();
    if (!output) {
        throw InternalError("cannot write database metadata: " + path.string());
    }
}

/**
 * @brief 将系统命令的字符串结果转换成统一的 WireResultSet。
 * @throws InternalError 上下文为空、行列不匹配或结果超过教学版缓存上限。
 */
void set_string_result(Context* context,
                       std::vector<std::string> column_names,
                       std::vector<std::vector<std::string>> row_values) {
    if (context == nullptr) {
        throw InternalError("utility result requires a request context");
    }

    std::vector<WireResultColumn> columns;
    columns.reserve(column_names.size());
    for (auto& name : column_names) {
        columns.push_back({.name = std::move(name), .type = TYPE_STRING});
    }
    context->result().set_columns(std::move(columns));

    for (auto& values : row_values) {
        std::vector<WireResultCell> row;
        row.reserve(values.size());
        for (auto& value : values) {
            row.push_back({.type = TYPE_STRING, .str_val = std::move(value)});
        }
        context->result().add_row(std::move(row));
    }
}

}  // namespace

bool SmManager::is_dir(const std::string& db_name) const { return fs::is_directory(database_path(db_name)); }

void SmManager::create_db(const std::string& db_name) const {
    const fs::path db_path = database_path(db_name);
    if (fs::exists(db_path)) {
        throw DatabaseExistsError(db_name);
    }

    if (!fs::create_directory(db_path)) {
        throw DatabaseExistsError(db_name);
    }

    try {
        DbMeta new_db;
        new_db.name_ = db_name;
        write_meta_file(db_path / DB_META_NAME, new_db);
        disk_manager_->create_file((db_path / LOG_FILE_NAME).string());
    } catch (...) {
        // 只回滚本次刚创建的目录，不触碰任何既有路径。
        std::error_code ignored;
        fs::remove_all(db_path, ignored);
        throw;
    }
}

void SmManager::drop_db(const std::string& db_name) {
    const fs::path db_path = database_path(db_name);
    if (!fs::is_directory(db_path) || !fs::is_regular_file(db_path / DB_META_NAME)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (db_.name_ == db_name) {
        throw InternalError("cannot drop an open database: " + db_name);
    }
    if (fs::remove_all(db_path) == 0) {
        throw DatabaseNotFoundError(db_name);
    }
}

void SmManager::open_db(const std::string& db_name) {
    const fs::path db_path = database_path(db_name);
    if (!fs::is_directory(db_path) || !fs::is_regular_file(db_path / DB_META_NAME)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }

    DbMeta new_db;
    {
        std::ifstream meta(DB_META_NAME);
        if (!meta) {
            chdir("..");
            throw InternalError("cannot open database metadata: " + db_name);
        }
        meta >> new_db;
    }

    // 打开所有表文件和索引文件；任一失败时回滚已打开的句柄并退出数据库目录。
    try {
        for (const auto& [tab_name, tab] : new_db.tabs_) {
            fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
            for (const auto& index : tab.indexes) {
                const std::string index_name = index_manager_->make_index_name(tab_name, index.cols);
                indexes_.emplace(index_name, index_manager_->open_index(tab_name, index.cols));
            }
        }
    } catch (...) {
        for (auto& [index_name, ih] : indexes_) {
            index_manager_->close_index(ih.get());
        }
        indexes_.clear();
        for (auto& [name, fh] : fhs_) {
            rm_manager_->close_file(fh.get());
        }
        fhs_.clear();
        chdir("..");
        throw;
    }
    db_ = std::move(new_db);
}

void SmManager::flush_meta() const { write_meta_file(DB_META_NAME, db_); }

void SmManager::show_database(Context* context) const { set_string_result(context, {"Database"}, {{db_.name_}}); }

void SmManager::close_db() {
    flush_meta();

    // 先关闭索引，再关闭表文件，最后清空内存目录
    for (auto& [index_name, ih] : indexes_) {
        index_manager_->close_index(ih.get());
    }
    indexes_.clear();
    for (auto& [tab_name, fh] : fhs_) {
        rm_manager_->close_file(fh.get());
    }
    fhs_.clear();
    db_ = DbMeta{};

    // 回到数据库目录的上一级
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

void SmManager::show_tables(Context* context) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(db_.tabs_.size());
    // C++20：`map | std::views::values` 只遍历 value（表元数据），不关心 key。
    // 等价于 for (const auto& [name, tab] : db_.tabs_) { ... 使用 tab ... }
    for (const auto& tab : db_.tabs_ | std::views::values) {
        rows.push_back({tab.name});
    }
    set_string_result(context, {"Tables"}, std::move(rows));
}

void SmManager::desc_table(const std::string& tab_name, Context* context) {
    const TabMeta& tab = db_.get_table(tab_name);

    std::vector<std::vector<std::string>> rows;
    rows.reserve(tab.cols.size());
    for (const auto& col : tab.cols) {
        rows.push_back({col.name, coltype2str(col.type), col.index ? "YES" : "NO"});
    }
    set_string_result(context, {"Field", "Type", "Index"}, std::move(rows));
}

void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // C++20：unordered_map::contains，避免 find/end 比较。
    if (fhs_.contains(tab_name)) {
        throw InternalError("Record handle already exists: " + tab_name);
    }
    if (col_defs.empty()) {
        throw InvalidRecordSizeError(0);
    }

    std::vector<std::string> col_names;
    col_names.reserve(col_defs.size());
    int record_size = 0;
    // C++17 结构化绑定：ColDef 是聚合类型，可拆成 name/type/len。
    // 等价于 for (const auto& col_def : col_defs) { 使用 col_def.name 等 }
    for (const auto& [name, type, len] : col_defs) {
        // C++20 ranges::find：直接对容器查找，无需 begin()/end()。
        if (std::ranges::find(col_names, name) != col_names.end()) {
            throw ColumnExistsError(name);
        }
        col_names.push_back(name);

        const bool fixed_length_mismatch = (type == TYPE_INT && len != static_cast<int>(sizeof(int))) ||
                                           (type == TYPE_FLOAT && len != static_cast<int>(sizeof(float)));
        if (len <= 0 || fixed_length_mismatch) {
            throw InvalidColLengthError(len);
        }
        if (len > RM_MAX_RECORD_SIZE) {
            throw InvalidRecordSizeError(len);
        }
        const int next_record_size = record_size + len;
        if (next_record_size > RM_MAX_RECORD_SIZE) {
            throw InvalidRecordSizeError(next_record_size);
        }
        record_size = next_record_size;
    }

    // 字段按声明顺序连续存放，offset 是字段在一条记录中的起始位置。
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    tab.cols.reserve(col_defs.size());
    // 同上：结构化绑定展开 ColDef 的三个成员。
    for (const auto& [name, type, len] : col_defs) {
        ColMeta col;
        col.tab_name = tab_name;
        col.name = name;
        col.type = type;
        col.len = len;
        col.offset = curr_offset;
        curr_offset += len;
        tab.cols.push_back(std::move(col));
    }

    // 先构造候选目录，只有文件句柄和 db.meta 都准备成功后才替换当前目录。
    DbMeta next_db = db_;
    next_db.tabs_[tab_name] = tab;
    rm_manager_->create_file(tab_name, record_size);

    try {
        fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
        write_meta_file(DB_META_NAME, next_db);
        db_ = std::move(next_db);
    } catch (...) {
        // 只清理本次建表产生的句柄和文件，再保留原目录供调用者继续使用。
        if (auto handle = fhs_.find(tab_name); handle != fhs_.end()) {
            rm_manager_->close_file(handle->second.get());
            fhs_.erase(handle);
        }
        rm_manager_->destroy_file(tab_name);
        throw;
    }
}

void SmManager::drop_table(const std::string& tab_name, Context* context) {
    TabMeta tab = db_.get_table(tab_name);  // 不存在时抛出 TableNotFoundError

    // 先删除表上的全部索引（drop_index 会同步维护元数据和索引文件）
    const std::vector<IndexMeta> indexes = tab.indexes;
    for (const auto& index : indexes) {
        drop_index(tab_name, index.cols, context);
    }

    // 关闭并删除记录文件
    rm_manager_->close_file(fhs_.at(tab_name).get());
    fhs_.erase(tab_name);
    rm_manager_->destroy_file(tab_name);

    db_.tabs_.erase(tab_name);
    flush_meta();
}

void SmManager::create_index(const std::string& tab_name,
                             const std::vector<std::string>& col_names,
                             bool unique,
                             Context* context) {
    TabMeta& tab = db_.get_table(tab_name);

    std::vector<ColMeta> cols;
    cols.reserve(col_names.size());
    for (const auto& col_name : col_names) {
        cols.push_back(*tab.get_col(col_name));  // 字段不存在时抛出 ColumnNotFoundError
    }
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    const IndexMeta meta = IndexMeta::make(tab_name, cols, unique);
    const std::string index_name = index_manager_->make_index_name(tab_name, cols);

    index_manager_->create_index(meta);
    std::unique_ptr<BPlusTree> ih;
    try {
        ih = index_manager_->open_index(tab_name, cols);

        // 表中已有记录也要写入新索引；UNIQUE 索引遇到重复 key 时抛出 DuplicateKeyError
        RmFileHandle* fh = fhs_.at(tab_name).get();
        RmScan scan(fh);
        while (!scan.is_end()) {
            const Rid rid = scan.rid();
            auto rec = fh->get_record(rid, context);
            std::vector<char> key = meta.make_key(rec->data);
            ih->insert_entry(key.data(), rid, context == nullptr ? nullptr : context->transaction());
            scan.next();
        }
    } catch (...) {
        // 建索引失败：清理索引文件和句柄，元数据保持不变，再把异常抛给上层
        if (ih != nullptr) {
            index_manager_->close_index(ih.get());
        }
        index_manager_->destroy_index(tab_name, cols);
        throw;
    }

    indexes_.emplace(index_name, std::move(ih));
    tab.indexes.push_back(meta);
    for (const auto& col : cols) {
        tab.get_col(col.name)->index = true;
    }
    flush_meta();
}

void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta& tab = db_.get_table(tab_name);
    std::vector<ColMeta> cols;
    cols.reserve(col_names.size());
    for (const auto& col_name : col_names) {
        cols.push_back(*tab.get_col(col_name));  // 字段不存在时抛出 ColumnNotFoundError
    }
    drop_index(tab_name, cols, context);
}

void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    TabMeta& tab = db_.get_table(tab_name);

    std::vector<std::string> col_names;
    col_names.reserve(cols.size());
    for (const auto& col : cols) {
        col_names.push_back(col.name);
    }
    tab.get_index_meta(col_names);  // 索引不存在时抛出 IndexNotFoundError

    // 关闭并删除索引文件
    const std::string index_name = index_manager_->make_index_name(tab_name, cols);
    if (auto it = indexes_.find(index_name); it != indexes_.end()) {
        index_manager_->close_index(it->second.get());
        indexes_.erase(it);
    }
    index_manager_->destroy_index(tab_name, cols);

    // 更新元数据：删除索引定义，并重新推导各字段的 index 标志
    tab.indexes.erase(tab.get_index_meta(col_names));
    for (auto& col : tab.cols) {
        col.index = false;
    }
    for (const auto& index : tab.indexes) {
        for (const auto& col : index.cols) {
            tab.get_col(col.name)->index = true;
        }
    }
    flush_meta();
}
