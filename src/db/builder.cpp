// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "db/builder.hpp"

#include <algorithm>

#include "db/filename.hpp"
#include "db/table_cache.hpp"

namespace strata {

TableFileWriter::TableFileWriter(Env* env, const TableOptions& options, std::string path)
    : env_(env), options_(options), path_(std::move(path)) {}

TableFileWriter::~TableFileWriter() {
    if (builder_ && !done_) builder_->abandon();
}

Status TableFileWriter::open() {
    Status s = env_->new_writable_file(path_, &file_);
    if (!s.is_ok()) return s;
    builder_ = std::make_unique<TableBuilder>(options_, file_.get());
    return Status::ok();
}

void TableFileWriter::add(std::string_view internal_key, std::string_view value) {
    if (builder_->num_entries() == 0) first_key_.assign(internal_key);
    last_key_.assign(internal_key);
    ParsedInternalKey parsed;
    if (parse_internal_key(internal_key, &parsed)) {
        if (parsed.type == ValueType::Deletion) ++deletions_;
        smallest_seq_ = std::min(smallest_seq_, parsed.sequence);
        largest_seq_ = std::max(largest_seq_, parsed.sequence);
    }
    builder_->add(internal_key, value);
}

std::uint64_t TableFileWriter::num_entries() const {
    return builder_ ? builder_->num_entries() : 0;
}
std::uint64_t TableFileWriter::file_size() const {
    return builder_ ? builder_->file_size() : 0;
}
Status TableFileWriter::status() const {
    return builder_ ? builder_->status() : Status::ok();
}

Status TableFileWriter::finish(FileMetaData* meta) {
    done_ = true;
    builder_->set_property("strata.num-deletions", std::to_string(deletions_));
    builder_->set_property("strata.smallest-sequence",
                           std::to_string(builder_->num_entries() == 0 ? 0 : smallest_seq_));
    builder_->set_property("strata.largest-sequence", std::to_string(largest_seq_));
    Status s = builder_->finish();
    if (s.is_ok()) s = file_->sync();
    if (s.is_ok()) s = file_->close();
    if (s.is_ok()) {
        meta->file_size = builder_->file_size();
        (void)meta->smallest.decode_from(first_key_);
        (void)meta->largest.decode_from(last_key_);
    }
    file_.reset();
    return s;
}

void TableFileWriter::abandon() {
    if (builder_ && !done_) builder_->abandon();
    done_ = true;
    if (file_) (void)file_->close();
    file_.reset();
}

Status build_table(const std::string& dbname, Env* env, const TableOptions& options, TableCache* table_cache,
                   Iterator* iter, FileMetaData* meta) {
    meta->file_size = 0;
    iter->seek_to_first();
    const std::string path = table_file_name(dbname, meta->number);

    Status s;
    if (iter->valid()) {
        TableFileWriter writer(env, options, path);
        s = writer.open();
        if (!s.is_ok()) return s;
        for (; iter->valid(); iter->next()) writer.add(iter->key(), iter->value());
        s = iter->status();
        if (s.is_ok()) {
            s = writer.finish(meta);
        } else {
            writer.abandon();
        }
        if (s.is_ok()) {
            // Make sure the new table is readable before it is installed.
            auto check = table_cache->new_iterator(ReadOptions(), meta->number, meta->file_size);
            s = check->status();
        }
    }

    if (!s.is_ok() || meta->file_size == 0) (void)env->remove_file(path);
    return s;
}

}  // namespace strata
