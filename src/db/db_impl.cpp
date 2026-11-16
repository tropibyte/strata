// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "db/db_impl.hpp"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdio>

#include "db/builder.hpp"
#include "db/db_iter.hpp"
#include "db/filename.hpp"
#include "db/log_reader.hpp"
#include "db/memtable.hpp"
#include "db/table_cache.hpp"
#include "db/version_set.hpp"
#include "db/write_batch_internal.hpp"
#include "table/merging_iterator.hpp"
#include "util/strutil.hpp"

namespace strata {

// A thread waiting to write. Lives on that thread's stack.
struct DBImpl::Writer {
    explicit Writer(WriteBatch* b, bool s) : batch(b), sync(s) {}

    WriteBatch* batch;
    bool sync;
    bool done = false;
    Status status;
    std::condition_variable cv;
};

// The output side of one compaction.
struct DBImpl::CompactionState {
    explicit CompactionState(Compaction* c) : compaction(c) {}

    struct Output {
        std::uint64_t number = 0;
        std::uint64_t file_size = 0;
        InternalKey smallest, largest;
    };

    Compaction* const compaction;

    // Entries older than this that are overwritten by a newer entry for the
    // same key are invisible to every snapshot and can be dropped.
    SequenceNumber smallest_snapshot = 0;

    std::vector<Output> outputs;
    std::unique_ptr<TableFileWriter> writer;  // current output file
    std::uint64_t total_bytes = 0;

    Output& current_output() { return outputs.back(); }
};

namespace {

template <class T>
T clip(T value, T low, T high) {
    return std::min(std::max(value, low), high);
}

constexpr std::size_t kMaxGroupBytes = 1u << 20;

}  // namespace

Options sanitize_options(const InternalKeyComparator* icmp, const Options& src) {
    Options r = src;
    r.comparator = icmp->user_comparator();
    if (r.env == nullptr) r.env = Env::default_env();
    r.num_levels = clip(r.num_levels, 2, 20);
    r.max_open_files = clip(r.max_open_files, 20, 50000);
    r.write_buffer_size = clip<std::size_t>(r.write_buffer_size, 1024, std::size_t{1} << 30);
    r.block_size = clip<std::size_t>(r.block_size, 256, std::size_t{4} << 20);
    r.block_restart_interval = clip(r.block_restart_interval, 1, 1024);
    r.bloom_bits_per_key = clip(r.bloom_bits_per_key, 0, 64);
    r.target_file_size = clip<std::size_t>(r.target_file_size, 1024, std::size_t{1} << 30);
    r.max_bytes_for_level_base = clip<std::uint64_t>(r.max_bytes_for_level_base, 1024, std::uint64_t{1} << 40);
    if (!(r.level_size_multiplier >= 2.0)) r.level_size_multiplier = 2.0;
    r.level0_compaction_trigger = std::max(1, r.level0_compaction_trigger);
    r.level0_slowdown_trigger = std::max(r.level0_compaction_trigger, r.level0_slowdown_trigger);
    r.level0_stop_trigger = std::max(r.level0_slowdown_trigger, r.level0_stop_trigger);
    r.max_memtable_level = clip(r.max_memtable_level, 0, r.num_levels - 1);
    return r;
}

DBImpl::DBImpl(const Options& raw_options, std::string dbname)
    : env_(raw_options.env != nullptr ? raw_options.env : Env::default_env()),
      internal_comparator_(raw_options.comparator != nullptr ? raw_options.comparator : bytewise_comparator()),
      options_(sanitize_options(&internal_comparator_, raw_options)),
      dbname_(std::move(dbname)),
      info_log_(raw_options.info_log),
      stats_(static_cast<std::size_t>(options_.num_levels)) {
    table_options_.comparator = &internal_comparator_;
    table_options_.block_size = options_.block_size;
    table_options_.block_restart_interval = options_.block_restart_interval;
    table_options_.bloom_bits_per_key = options_.bloom_bits_per_key;
    table_options_.compression = options_.compression;
    table_options_.filter_key = &filter_user_key;

    // Keep a few descriptors for logs, the MANIFEST and the LOG file.
    const int table_cache_size = options_.max_open_files - 10;
    table_cache_ =
        std::make_unique<TableCache>(dbname_, env_, table_options_, table_cache_size, options_.block_cache_bytes);
    versions_ = std::make_unique<VersionSet>(dbname_, &options_, table_cache_.get(), &internal_comparator_);
}

DBImpl::~DBImpl() {
    {
        Lock lock(mutex_);
        shutting_down_.store(true, std::memory_order_release);
        background_work_cv_.notify_all();
    }
    if (background_thread_.joinable()) background_thread_.join();

    if (logfile_) (void)logfile_->close();
    log_.reset();
    logfile_.reset();
    versions_.reset();
    table_cache_.reset();
    if (db_lock_) (void)env_->unlock_file(std::move(db_lock_));
    owned_info_log_.reset();
}

// ---------------------------------------------------------------------------
// Opening

void DBImpl::open_info_log() {
    if (info_log_ != nullptr) return;
    (void)env_->create_dir(dbname_);
    (void)env_->rename_file(info_log_file_name(dbname_), old_info_log_file_name(dbname_));
    if (env_->new_logger(info_log_file_name(dbname_), &owned_info_log_).is_ok()) info_log_ = owned_info_log_.get();
}

Status DBImpl::new_db() {
    VersionEdit edit;
    edit.set_comparator_name(internal_comparator_.user_comparator()->name());
    edit.set_log_number(0);
    edit.set_next_file(2);
    edit.set_last_sequence(0);

    const std::string manifest = descriptor_file_name(dbname_, 1);
    std::unique_ptr<WritableFile> file;
    Status s = env_->new_writable_file(manifest, &file);
    if (!s.is_ok()) return s;
    {
        log::Writer log(file.get());
        std::string record;
        edit.encode_to(record);
        s = log.add_record(record);
        if (s.is_ok()) s = file->sync();
        if (s.is_ok()) s = file->close();
    }
    file.reset();
    if (s.is_ok()) {
        s = set_current_file(env_, dbname_, 1);
    } else {
        (void)env_->remove_file(manifest);
    }
    return s;
}

Status DBImpl::recover(VersionEdit* edit, bool* save_manifest, Lock& lock) {
    // The directory may already exist; any real problem shows up below.
    (void)env_->create_dir(dbname_);
    Status s = env_->lock_file(lock_file_name(dbname_), &db_lock_);
    if (!s.is_ok()) return s;

    open_info_log();

    if (!env_->file_exists(current_file_name(dbname_))) {
        if (!options_.create_if_missing) {
            return Status::invalid_argument(dbname_, "does not exist (create_if_missing is false)");
        }
        log_printf(info_log_, "Creating DB %s", dbname_.c_str());
        s = new_db();
        if (!s.is_ok()) return s;
    } else if (options_.error_if_exists) {
        return Status::invalid_argument(dbname_, "exists (error_if_exists is true)");
    }

    s = versions_->recover();
    if (!s.is_ok()) return s;
    // A new MANIFEST is always written after opening (see DB::open), which
    // also keeps the old one from growing without bound.
    *save_manifest = true;

    // Every table the MANIFEST lists must exist. Logs at or after the
    // MANIFEST's log number hold writes not yet in any table: replay them.
    std::set<std::uint64_t> expected;
    versions_->add_live_files(&expected);
    std::vector<std::string> names;
    s = env_->get_children(dbname_, &names);
    if (!s.is_ok()) return s;
    std::vector<std::uint64_t> logs;
    for (const auto& name : names) {
        std::uint64_t number = 0;
        FileType type;
        if (!parse_file_name(name, &number, &type)) continue;
        if (type == FileType::Table) expected.erase(number);
        if (type == FileType::Log && number >= versions_->log_number()) logs.push_back(number);
    }
    if (!expected.empty()) {
        return Status::corruption(std::to_string(expected.size()) + " missing table files, e.g.",
                                  table_file_name(dbname_, *expected.begin()));
    }

    std::sort(logs.begin(), logs.end());
    // The logs' numbers were handed out before the MANIFEST last recorded
    // the next file number; don't give them to the tables made below.
    for (std::uint64_t number : logs) versions_->mark_file_number_used(number);
    SequenceNumber max_sequence = 0;
    for (std::uint64_t number : logs) {
        s = recover_log_file(number, edit, &max_sequence, save_manifest, lock);
        if (!s.is_ok()) return s;
    }
    if (versions_->last_sequence() < max_sequence) versions_->set_last_sequence(max_sequence);
    return Status::ok();
}

namespace {

class LogReporter final : public log::Reader::Reporter {
public:
    LogReporter(Logger* info_log, std::string fname, Status* status)
        : info_log_(info_log), fname_(std::move(fname)), status_(status) {}

    void corruption(std::size_t bytes, const Status& s) override {
        log_printf(info_log_, "%s%s: dropping %llu bytes; %s", status_ == nullptr ? "(ignoring error) " : "",
                   fname_.c_str(), static_cast<unsigned long long>(bytes), s.to_string().c_str());
        if (status_ != nullptr && status_->is_ok()) *status_ = s;
    }

private:
    Logger* info_log_;
    std::string fname_;
    Status* status_;  // null: ignore errors
};

}  // namespace

Status DBImpl::recover_log_file(std::uint64_t log_number, VersionEdit* edit, SequenceNumber* max_sequence,
                                bool* save_manifest, Lock& lock) {
    const std::string fname = log_file_name(dbname_, log_number);
    std::unique_ptr<SequentialFile> file;
    Status status = env_->new_sequential_file(fname, &file);
    if (!status.is_ok()) return status;

    // Without paranoid checks, a damaged record is skipped (and logged) and
    // recovery goes on with the rest of the log.
    LogReporter reporter(info_log_, fname, options_.paranoid_checks ? &status : nullptr);
    log::Reader reader(file.get(), &reporter, /*checksum=*/true);
    log_printf(info_log_, "Recovering log #%" PRIu64, log_number);

    std::string record;
    WriteBatch batch;
    std::shared_ptr<MemTable> mem;
    while (reader.read_record(&record) && status.is_ok()) {
        if (record.size() < WriteBatchInternal::kHeaderSize) {
            reporter.corruption(record.size(), Status::corruption("log record too small"));
            continue;
        }
        WriteBatchInternal::set_contents(batch, record);
        if (!mem) mem = std::make_shared<MemTable>(internal_comparator_);
        Status s = WriteBatchInternal::insert_into(batch, mem.get());
        if (!s.is_ok()) {
            // A record that passed its checksum but does not parse.
            reporter.corruption(record.size(), s);
            continue;
        }
        const SequenceNumber last = WriteBatchInternal::sequence(batch) + WriteBatchInternal::count(batch) - 1;
        if (WriteBatchInternal::count(batch) > 0 && last > *max_sequence) *max_sequence = last;

        if (mem->approximate_memory_usage() > options_.write_buffer_size) {
            *save_manifest = true;
            status = write_level0_table(mem.get(), edit, nullptr, lock);
            mem.reset();
        }
    }

    if (status.is_ok() && mem) {
        *save_manifest = true;
        status = write_level0_table(mem.get(), edit, nullptr, lock);
    }
    return status;
}

Status DB::open(const Options& options, const std::string& path, std::unique_ptr<DB>* out) {
    out->reset();
    auto impl = std::make_unique<DBImpl>(options, path);
    DBImpl::Lock lock(impl->mutex_);

    VersionEdit edit;
    bool save_manifest = false;
    Status s = impl->recover(&edit, &save_manifest, lock);

    if (s.is_ok()) {
        // Start a fresh write-ahead log.
        const std::uint64_t log_number = impl->versions_->new_file_number();
        std::unique_ptr<WritableFile> lfile;
        s = impl->env_->new_writable_file(log_file_name(path, log_number), &lfile);
        if (s.is_ok()) {
            edit.set_log_number(log_number);
            impl->logfile_ = std::move(lfile);
            impl->logfile_number_ = log_number;
            impl->log_ = std::make_unique<log::Writer>(impl->logfile_.get());
            impl->mem_ = std::make_shared<MemTable>(impl->internal_comparator_);
        }
    }
    if (s.is_ok() && save_manifest) s = impl->versions_->log_and_apply(&edit, lock);
    if (s.is_ok()) {
        impl->remove_obsolete_files(lock);
        impl->background_thread_ = std::thread([db = impl.get()] { db->background_thread_main(); });
        impl->maybe_schedule_compaction();
        log_printf(impl->info_log_, "Opened %s: %s", path.c_str(), impl->versions_->level_summary().c_str());
    }
    lock.unlock();

    if (s.is_ok()) *out = std::move(impl);
    return s;
}

// ---------------------------------------------------------------------------
// Writes

Status DBImpl::put(const WriteOptions& options, std::string_view key, std::string_view value) {
    WriteBatch batch;
    batch.put(key, value);
    return write(options, &batch);
}

Status DBImpl::remove(const WriteOptions& options, std::string_view key) {
    WriteBatch batch;
    batch.remove(key);
    return write(options, &batch);
}

Status DBImpl::write(const WriteOptions& options, WriteBatch* updates) {
    Writer w(updates, options.sync);
    Lock lock(mutex_);
    writers_.push_back(&w);
    while (!w.done && &w != writers_.front()) w.cv.wait(lock);
    if (w.done) return w.status;  // a group leader wrote our batch

    // We lead a group. A null batch (from flush()) forces a memtable switch.
    Status status = make_room_for_write(updates == nullptr, lock);
    SequenceNumber last_sequence = versions_->last_sequence();
    Writer* last_writer = &w;
    if (status.is_ok() && updates != nullptr) {
        WriteBatch* group = build_batch_group(&last_writer);
        WriteBatchInternal::set_sequence(*group, last_sequence + 1);
        last_sequence += WriteBatchInternal::count(*group);

        // Only the front writer touches the log and the memtable, so both
        // can be written without the mutex while others queue up.
        {
            lock.unlock();
            status = log_->add_record(WriteBatchInternal::contents(*group));
            bool sync_error = false;
            if (status.is_ok() && options.sync) {
                status = logfile_->sync();
                if (!status.is_ok()) sync_error = true;
            }
            if (status.is_ok()) status = WriteBatchInternal::insert_into(*group, mem_.get());
            lock.lock();
            if (sync_error) {
                // The log may or may not hold the group; stop accepting
                // writes rather than risk a recovered state that differs
                // from what readers already saw.
                record_background_error(status);
            }
        }
        if (group == &group_batch_) group_batch_.clear();
        versions_->set_last_sequence(last_sequence);
    }

    // Wake every writer whose batch went into the group.
    for (;;) {
        Writer* ready = writers_.front();
        writers_.pop_front();
        if (ready != &w) {
            ready->status = status;
            ready->done = true;
            ready->cv.notify_one();
        }
        if (ready == last_writer) break;
    }
    if (!writers_.empty()) writers_.front()->cv.notify_one();
    return status;
}

WriteBatch* DBImpl::build_batch_group(Writer** last_writer) {
    assert(!writers_.empty());
    Writer* first = writers_.front();
    WriteBatch* result = first->batch;
    assert(result != nullptr);

    // Keep small writes fast: a small first batch limits the group, so it
    // isn't held up behind a lot of data.
    std::size_t size = WriteBatchInternal::byte_size(*first->batch);
    std::size_t max_size = kMaxGroupBytes;
    if (size <= (128u << 10)) max_size = size + (128u << 10);

    *last_writer = first;
    for (auto it = std::next(writers_.begin()); it != writers_.end(); ++it) {
        Writer* w = *it;
        // A sync write must not ride along with a non-sync leader.
        if (w->sync && !first->sync) break;
        // A null batch (a forced flush) ends the group.
        if (w->batch == nullptr) break;
        size += WriteBatchInternal::byte_size(*w->batch);
        if (size > max_size) break;

        if (result == first->batch) {
            // Don't modify the caller's batch: copy into the scratch batch.
            result = &group_batch_;
            assert(WriteBatchInternal::count(*result) == 0);
            WriteBatchInternal::append(*result, *first->batch);
        }
        WriteBatchInternal::append(*result, *w->batch);
        *last_writer = w;
    }
    return result;
}

Status DBImpl::make_room_for_write(bool force, Lock& lock) {
    assert(!writers_.empty());
    bool allow_delay = !force;
    Status s;
    for (;;) {
        if (!bg_error_.is_ok()) {
            s = bg_error_;
            break;
        }
        if (allow_delay && versions_->num_level_files(0) >= options_.level0_slowdown_trigger) {
            // Close to the stop limit: slow each write by 1 ms instead of
            // stalling one write for seconds. Once per write.
            lock.unlock();
            env_->sleep_for_micros(1000);
            allow_delay = false;
            lock.lock();
            continue;
        }
        // Room in the memtable. An empty one always has room: with a tiny
        // write buffer the arena's first block alone can exceed it.
        if (!force && (mem_->num_entries() == 0 || mem_->approximate_memory_usage() <= options_.write_buffer_size)) {
            break;
        }
        if (imm_ != nullptr) {
            // The previous memtable is still being flushed.
            log_printf(info_log_, "Current memtable full; waiting...");
            background_done_cv_.wait(lock);
            continue;
        }
        if (versions_->num_level_files(0) >= options_.level0_stop_trigger) {
            log_printf(info_log_, "Too many L0 files; waiting...");
            background_done_cv_.wait(lock);
            continue;
        }

        // Switch to a new memtable and log; flush the old one.
        const std::uint64_t new_log_number = versions_->new_file_number();
        std::unique_ptr<WritableFile> lfile;
        s = env_->new_writable_file(log_file_name(dbname_, new_log_number), &lfile);
        if (!s.is_ok()) {
            versions_->reuse_file_number(new_log_number);
            break;
        }
        Status close_status = logfile_->close();
        if (!close_status.is_ok()) {
            // The old log may be missing writes; don't accept more.
            record_background_error(close_status);
        }
        logfile_ = std::move(lfile);
        logfile_number_ = new_log_number;
        log_ = std::make_unique<log::Writer>(logfile_.get());
        imm_ = mem_;
        has_imm_.store(true, std::memory_order_release);
        mem_ = std::make_shared<MemTable>(internal_comparator_);
        force = false;  // the next pass finds room in the new memtable
        maybe_schedule_compaction();
    }
    return s;
}

// ---------------------------------------------------------------------------
// Reads

SequenceNumber DBImpl::snapshot_sequence(const ReadOptions& options) const {
    if (options.snapshot != nullptr) return static_cast<const SnapshotImpl*>(options.snapshot)->sequence();
    return versions_->last_sequence();
}

Status DBImpl::get(const ReadOptions& options, std::string_view key, std::string* value) {
    Lock lock(mutex_);
    const SequenceNumber snapshot = snapshot_sequence(options);
    const std::shared_ptr<MemTable> mem = mem_;
    const std::shared_ptr<MemTable> imm = imm_;
    const std::shared_ptr<Version> current = versions_->current();
    lock.unlock();

    const LookupKey lkey(key, snapshot);
    Status s;
    if (mem->get(lkey, value, &s)) return s;
    if (imm != nullptr && imm->get(lkey, value, &s)) return s;
    return current->get(options, lkey, value);
}

std::unique_ptr<Iterator> DBImpl::new_iterator(const ReadOptions& options) {
    Lock lock(mutex_);
    const SequenceNumber sequence = snapshot_sequence(options);
    std::vector<std::unique_ptr<Iterator>> children;
    std::vector<std::shared_ptr<const void>> pins;
    children.push_back(mem_->new_iterator());
    pins.push_back(mem_);
    if (imm_ != nullptr) {
        children.push_back(imm_->new_iterator());
        pins.push_back(imm_);
    }
    std::shared_ptr<Version> current = versions_->current();
    pins.push_back(current);
    lock.unlock();

    // Opening table files happens outside the mutex.
    current->add_iterators(options, &children);
    auto internal = new_merging_iterator(&internal_comparator_, std::move(children));
    return new_db_iterator(internal_comparator_.user_comparator(), std::move(internal), sequence, std::move(pins));
}

const Snapshot* DBImpl::get_snapshot() {
    Lock lock(mutex_);
    return snapshots_.create(versions_->last_sequence());
}

void DBImpl::release_snapshot(const Snapshot* snapshot) {
    if (snapshot == nullptr) return;
    Lock lock(mutex_);
    snapshots_.release(static_cast<const SnapshotImpl*>(snapshot));
}

// ---------------------------------------------------------------------------
// Background work

void DBImpl::background_thread_main() {
    Lock lock(mutex_);
    for (;;) {
        background_work_cv_.wait(
            lock, [this] { return shutting_down_.load(std::memory_order_acquire) || background_scheduled_; });
        if (shutting_down_.load(std::memory_order_acquire)) break;

        background_compaction(lock);
        background_scheduled_ = false;
        // The job may have produced more work (another level now too big).
        maybe_schedule_compaction();
        background_done_cv_.notify_all();
    }
    background_scheduled_ = false;
    background_done_cv_.notify_all();
}

void DBImpl::maybe_schedule_compaction() {
    if (background_scheduled_) return;
    if (shutting_down_.load(std::memory_order_acquire)) return;
    if (!bg_error_.is_ok()) return;
    if (imm_ == nullptr && manual_compaction_ == nullptr && !versions_->needs_compaction()) return;
    background_scheduled_ = true;
    background_work_cv_.notify_one();
}

void DBImpl::record_background_error(const Status& s) {
    if (bg_error_.is_ok() && !s.is_ok()) {
        bg_error_ = s;
        log_printf(info_log_, "Background error: %s", s.to_string().c_str());
        background_done_cv_.notify_all();
    }
}

void DBImpl::background_compaction(Lock& lock) {
    if (imm_ != nullptr) {
        compact_memtable(lock);
        return;
    }

    std::unique_ptr<Compaction> c;
    ManualCompaction* manual = manual_compaction_;
    InternalKey manual_end;
    if (manual != nullptr) {
        c = versions_->compact_range(manual->level, manual->begin ? &*manual->begin : nullptr,
                                     manual->end ? &*manual->end : nullptr);
        manual->done = (c == nullptr);
        if (c != nullptr) manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
        log_printf(info_log_, "Manual compaction at level-%d from %s .. %s; will stop at %s", manual->level,
                   manual->begin ? manual->begin->debug_string().c_str() : "(begin)",
                   manual->end ? manual->end->debug_string().c_str() : "(end)",
                   manual->done ? "(end)" : manual_end.debug_string().c_str());
    } else {
        c = versions_->pick_compaction();
    }

    Status status;
    if (c == nullptr) {
        // Nothing to do.
    } else if (manual == nullptr && c->is_trivial_move()) {
        // Move the file down a level by editing the MANIFEST only.
        const FileMetaPtr f = c->input(0, 0);
        c->edit()->remove_file(c->level(), f->number);
        c->edit()->add_file(c->level() + 1, *f);
        status = versions_->log_and_apply(c->edit(), lock);
        if (!status.is_ok()) record_background_error(status);
        log_printf(info_log_, "Moved #%" PRIu64 " to level-%d %" PRIu64 " bytes %s: %s", f->number, c->level() + 1,
                   f->file_size, status.to_string().c_str(), versions_->level_summary().c_str());
    } else {
        CompactionState compact(c.get());
        status = do_compaction_work(&compact, lock);
        if (!status.is_ok()) record_background_error(status);
        cleanup_compaction(&compact);
        c->release_inputs();
        remove_obsolete_files(lock);
    }
    c.reset();

    if (!status.is_ok() && !shutting_down_.load(std::memory_order_acquire)) {
        log_printf(info_log_, "Compaction error: %s", status.to_string().c_str());
    }

    if (manual != nullptr) {
        if (!status.is_ok()) manual->done = true;
        if (!manual->done) {
            // Only part of the range was compacted; continue after it.
            manual->begin = manual_end;
        }
        manual_compaction_ = nullptr;
    }
}

void DBImpl::compact_memtable(Lock& lock, bool allow_push_down) {
    assert(imm_ != nullptr);
    VersionEdit edit;
    const std::shared_ptr<Version> base = versions_->current();
    const std::shared_ptr<MemTable> imm = imm_;
    Status s = write_level0_table(imm.get(), &edit, allow_push_down ? base.get() : nullptr, lock);
    if (s.is_ok() && shutting_down_.load(std::memory_order_acquire)) {
        s = Status::io_error("deleting DB during memtable compaction");
    }
    if (s.is_ok()) {
        // Logs older than the current one are no longer needed: their
        // contents are now in the table just written.
        edit.set_log_number(logfile_number_);
        s = versions_->log_and_apply(&edit, lock);
    }
    if (s.is_ok()) {
        imm_.reset();
        has_imm_.store(false, std::memory_order_release);
        remove_obsolete_files(lock);
    } else {
        record_background_error(s);
    }
}

Status DBImpl::write_level0_table(MemTable* mem, VersionEdit* edit, const Version* base, Lock& lock) {
    const std::uint64_t start_micros = env_->now_micros();
    FileMetaData meta;
    meta.number = versions_->new_file_number();
    pending_outputs_.insert(meta.number);
    auto iter = mem->new_iterator();
    log_printf(info_log_, "Level-0 table #%" PRIu64 ": started", meta.number);

    Status s;
    {
        lock.unlock();
        s = build_table(dbname_, env_, table_options_, table_cache_.get(), iter.get(), &meta);
        lock.lock();
    }
    log_printf(info_log_, "Level-0 table #%" PRIu64 ": %" PRIu64 " bytes %s", meta.number, meta.file_size,
               s.to_string().c_str());
    pending_outputs_.erase(meta.number);

    // An empty memtable makes no file.
    int level = 0;
    if (s.is_ok() && meta.file_size > 0) {
        if (base != nullptr) {
            level = base->pick_level_for_memtable_output(meta.smallest.user_key(), meta.largest.user_key());
        }
        edit->add_file(level, meta);
    }

    LevelStats stats;
    stats.micros = env_->now_micros() - start_micros;
    stats.bytes_written = meta.file_size;
    stats.count = 1;
    stats_[static_cast<std::size_t>(level)].add(stats);
    return s;
}

Status DBImpl::open_compaction_output_file(CompactionState* compact, Lock& lock) {
    std::uint64_t number;
    {
        lock.lock();
        number = versions_->new_file_number();
        pending_outputs_.insert(number);
        CompactionState::Output out;
        out.number = number;
        compact->outputs.push_back(out);
        lock.unlock();
    }
    compact->writer = std::make_unique<TableFileWriter>(env_, table_options_, table_file_name(dbname_, number));
    return compact->writer->open();
}

Status DBImpl::finish_compaction_output_file(CompactionState* compact) {
    assert(compact->writer != nullptr);
    auto& out = compact->current_output();
    const std::uint64_t entries = compact->writer->num_entries();
    FileMetaData meta;
    Status s = compact->writer->finish(&meta);
    if (s.is_ok()) {
        out.file_size = meta.file_size;
        out.smallest = meta.smallest;
        out.largest = meta.largest;
        compact->total_bytes += meta.file_size;
    }
    compact->writer.reset();

    if (s.is_ok() && entries > 0) {
        // Check that the file is usable before installing it.
        auto iter = table_cache_->new_iterator(ReadOptions(), out.number, out.file_size);
        s = iter->status();
        if (s.is_ok()) {
            log_printf(info_log_, "Generated table #%" PRIu64 "@%d: %" PRIu64 " keys, %" PRIu64 " bytes", out.number,
                       compact->compaction->level(), entries, out.file_size);
        }
    }
    return s;
}

Status DBImpl::install_compaction_results(CompactionState* compact, Lock& lock) {
    Compaction* c = compact->compaction;
    log_printf(info_log_, "Compacted %d@%d + %d@%d files => %" PRIu64 " bytes", c->num_input_files(0), c->level(),
               c->num_input_files(1), c->level() + 1, compact->total_bytes);
    c->add_input_deletions(c->edit());
    const int out_level = c->level() + 1;
    for (const auto& out : compact->outputs) {
        if (out.file_size == 0) continue;
        c->edit()->add_file(out_level, out.number, out.file_size, out.smallest, out.largest);
    }
    return versions_->log_and_apply(c->edit(), lock);
}

void DBImpl::cleanup_compaction(CompactionState* compact) {
    if (compact->writer != nullptr) {
        // Only on error: the file is abandoned and later deleted as obsolete.
        compact->writer->abandon();
        compact->writer.reset();
    }
    for (const auto& out : compact->outputs) pending_outputs_.erase(out.number);
}

Status DBImpl::do_compaction_work(CompactionState* compact, Lock& lock) {
    const std::uint64_t start_micros = env_->now_micros();
    std::uint64_t imm_micros = 0;  // time spent flushing memtables meanwhile
    Compaction* c = compact->compaction;

    log_printf(info_log_, "Compacting %d@%d + %d@%d files", c->num_input_files(0), c->level(), c->num_input_files(1),
               c->level() + 1);
    assert(versions_->num_level_files(c->level()) > 0);
    assert(compact->writer == nullptr);
    compact->smallest_snapshot = snapshots_.empty() ? versions_->last_sequence() : snapshots_.oldest();
    const Comparator* ucmp = internal_comparator_.user_comparator();

    // Work without the mutex. The compaction's inputs are fixed and the
    // table cache is thread-safe, so opening the inputs needs no lock.
    lock.unlock();
    auto input = versions_->make_input_iterator(c);

    input->seek_to_first();
    Status status;
    std::string current_user_key;
    bool has_current_user_key = false;
    SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
    std::string last_output_user_key;  // user key of the last entry written

    while (input->valid() && !shutting_down_.load(std::memory_order_acquire)) {
        // A full memtable stalls writers; flush it before going on.
        if (has_imm_.load(std::memory_order_relaxed)) {
            const std::uint64_t imm_start = env_->now_micros();
            lock.lock();
            if (imm_ != nullptr) {
                // A compaction's outputs can span gaps between its inputs, so
                // a flushed table must not be pushed into its output level.
                compact_memtable(lock, /*allow_push_down=*/false);
                background_done_cv_.notify_all();  // writers may be waiting
            }
            lock.unlock();
            imm_micros += env_->now_micros() - imm_start;
        }

        const std::string_view key = input->key();

        // Decide whether to start a new output file before this entry.
        // Never between two entries of one user key: a later compaction
        // could then move the newer one below the older.
        const bool stop_before = c->should_stop_before(key);
        if (compact->writer != nullptr && !compact->writer->empty()) {
            const bool full = compact->writer->file_size() >= c->max_output_file_size();
            if ((stop_before || full) && ucmp->compare(filter_user_key(key), last_output_user_key) != 0) {
                status = finish_compaction_output_file(compact);
                if (!status.is_ok()) break;
            }
        }

        // Decide whether this entry can be dropped.
        bool drop = false;
        ParsedInternalKey ikey;
        if (!parse_internal_key(key, &ikey)) {
            // Keep damaged keys rather than lose them silently.
            current_user_key.clear();
            has_current_user_key = false;
            last_sequence_for_key = kMaxSequenceNumber;
        } else {
            if (!has_current_user_key || ucmp->compare(ikey.user_key, current_user_key) != 0) {
                // First (newest) entry of this user key.
                current_user_key.assign(ikey.user_key);
                has_current_user_key = true;
                last_sequence_for_key = kMaxSequenceNumber;
            }
            if (last_sequence_for_key <= compact->smallest_snapshot) {
                // A newer entry for this key is visible to every snapshot,
                // so this one can never be seen.
                drop = true;
            } else if (ikey.type == ValueType::Deletion && ikey.sequence <= compact->smallest_snapshot &&
                       c->is_base_level_for_key(ikey.user_key)) {
                // A deletion marker with nothing older below it to hide, and
                // no snapshot that could see what it hides here: the older
                // entries of this key in this compaction are dropped by the
                // rule above, so the marker can go too.
                drop = true;
            }
            last_sequence_for_key = ikey.sequence;
        }

        if (!drop) {
            if (compact->writer == nullptr) {
                status = open_compaction_output_file(compact, lock);
                if (!status.is_ok()) break;
            }
            compact->writer->add(key, input->value());
            last_output_user_key.assign(filter_user_key(key));
        }

        input->next();
    }

    if (status.is_ok() && shutting_down_.load(std::memory_order_acquire)) {
        status = Status::io_error("deleting DB during compaction");
    }
    if (status.is_ok() && compact->writer != nullptr) status = finish_compaction_output_file(compact);
    if (status.is_ok()) status = input->status();
    input.reset();

    LevelStats stats;
    stats.micros = env_->now_micros() - start_micros - imm_micros;
    stats.bytes_read = c->input_bytes();
    stats.bytes_written = compact->total_bytes;
    stats.count = 1;

    lock.lock();
    stats_[static_cast<std::size_t>(c->level() + 1)].add(stats);
    if (status.is_ok()) status = install_compaction_results(compact, lock);
    log_printf(info_log_, "Compacted to: %s %s", versions_->level_summary().c_str(), status.to_string().c_str());
    return status;
}

void DBImpl::remove_obsolete_files(Lock& lock) {
    if (!bg_error_.is_ok()) {
        // After an error we cannot tell whether a new version was installed,
        // so we cannot tell which files are obsolete.
        return;
    }
    std::set<std::uint64_t> live = pending_outputs_;
    versions_->add_live_files(&live);

    std::vector<std::string> names;
    (void)env_->get_children(dbname_, &names);
    std::vector<std::string> to_delete;
    for (const auto& name : names) {
        std::uint64_t number = 0;
        FileType type;
        if (!parse_file_name(name, &number, &type)) continue;
        bool keep = true;
        switch (type) {
            case FileType::Log:
                keep = number >= versions_->log_number() || number == logfile_number_;
                break;
            case FileType::Descriptor:
                keep = number >= versions_->manifest_file_number();
                break;
            case FileType::Table:
                keep = live.count(number) != 0;
                break;
            case FileType::Temp:
                keep = live.count(number) != 0;
                break;
            case FileType::Current:
            case FileType::DbLock:
            case FileType::InfoLog:
                keep = true;
                break;
        }
        if (!keep) {
            to_delete.push_back(name);
            if (type == FileType::Table) table_cache_->evict(number);
            log_printf(info_log_, "Delete %s", name.c_str());
        }
    }

    // Deleting can be slow; other threads may proceed meanwhile. Only the
    // background thread (or open) gets here, so no one else deletes files.
    lock.unlock();
    for (const auto& name : to_delete) (void)env_->remove_file(dbname_ + "/" + name);
    lock.lock();
}

// ---------------------------------------------------------------------------
// Manual compaction and flush

Status DBImpl::flush() {
    {
        Lock lock(mutex_);
        if (mem_->num_entries() == 0 && imm_ == nullptr) return bg_error_;
    }
    Status s = write(WriteOptions(), nullptr);  // forces a memtable switch
    if (!s.is_ok()) return s;
    Lock lock(mutex_);
    while (imm_ != nullptr && bg_error_.is_ok()) background_done_cv_.wait(lock);
    if (imm_ != nullptr) return bg_error_;
    return Status::ok();
}

Status DBImpl::run_manual_compaction(int level, std::optional<std::string_view> begin,
                                     std::optional<std::string_view> end) {
    assert(level >= 0 && level + 1 < options_.num_levels);
    ManualCompaction manual;
    manual.level = level;
    if (begin) manual.begin = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    if (end) manual.end = InternalKey(*end, 0, ValueType::Deletion);

    Lock lock(mutex_);
    while (!manual.done && !shutting_down_.load(std::memory_order_acquire) && bg_error_.is_ok()) {
        if (manual_compaction_ == nullptr) {
            manual_compaction_ = &manual;
            maybe_schedule_compaction();
        }
        background_done_cv_.wait(lock);
    }
    // If the background thread is working on our request, wait for it to let
    // go: `manual` lives on this stack frame.
    while (manual_compaction_ == &manual && background_scheduled_) background_done_cv_.wait(lock);
    if (manual_compaction_ == &manual) manual_compaction_ = nullptr;
    return bg_error_;
}

Status DBImpl::compact_range(std::optional<std::string_view> begin, std::optional<std::string_view> end) {
    int max_level_with_files = 1;
    {
        Lock lock(mutex_);
        const auto base = versions_->current();
        const std::string_view* b = begin ? &*begin : nullptr;
        const std::string_view* e = end ? &*end : nullptr;
        for (int level = 1; level < options_.num_levels; ++level) {
            if (base->overlap_in_level(level, b, e)) max_level_with_files = level;
        }
    }
    Status s = flush();
    if (!s.is_ok()) return s;
    for (int level = 0; level < max_level_with_files; ++level) {
        s = run_manual_compaction(level, begin, end);
        if (!s.is_ok()) return s;
    }
    return Status::ok();
}

Status DBImpl::test_compact_range(int level, std::optional<std::string_view> begin,
                                  std::optional<std::string_view> end) {
    return run_manual_compaction(level, begin, end);
}

Status DBImpl::test_wait_for_background() {
    Lock lock(mutex_);
    while (background_scheduled_ && bg_error_.is_ok()) background_done_cv_.wait(lock);
    return bg_error_;
}

std::string DBImpl::test_files_per_level() {
    Lock lock(mutex_);
    std::string r;
    for (int level = 0; level < options_.num_levels; ++level) {
        if (level > 0) r += ",";
        r += std::to_string(versions_->num_level_files(level));
    }
    return r;
}

std::uint64_t DBImpl::test_max_next_level_overlapping_bytes() {
    Lock lock(mutex_);
    return versions_->max_next_level_overlapping_bytes();
}

// ---------------------------------------------------------------------------
// Introspection

bool DBImpl::get_property(std::string_view property, std::string* value) {
    value->clear();
    Lock lock(mutex_);
    constexpr std::string_view kPrefix = "strata.";
    if (property.substr(0, kPrefix.size()) != kPrefix) return false;
    std::string_view name = property.substr(kPrefix.size());

    constexpr std::string_view kNumFilesAtLevel = "num-files-at-level";
    if (name.substr(0, kNumFilesAtLevel.size()) == kNumFilesAtLevel) {
        std::string_view rest = name.substr(kNumFilesAtLevel.size());
        std::uint64_t level = 0;
        if (!consume_decimal(rest, &level) || !rest.empty() ||
            level >= static_cast<std::uint64_t>(options_.num_levels)) {
            return false;
        }
        *value = std::to_string(versions_->num_level_files(static_cast<int>(level)));
        return true;
    }

    if (name == "stats") {
        char buf[256];
        *value = "                               Compactions\n";
        *value += "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n";
        *value += "--------------------------------------------------\n";
        for (int level = 0; level < options_.num_levels; ++level) {
            const int files = versions_->num_level_files(level);
            const auto& st = stats_[static_cast<std::size_t>(level)];
            if (st.micros == 0 && files == 0) continue;
            std::snprintf(buf, sizeof(buf), "%3d %8d %8.1f %9.1f %8.1f %9.1f\n", level, files,
                          static_cast<double>(versions_->num_level_bytes(level)) / 1048576.0,
                          static_cast<double>(st.micros) / 1e6, static_cast<double>(st.bytes_read) / 1048576.0,
                          static_cast<double>(st.bytes_written) / 1048576.0);
            *value += buf;
        }
        std::snprintf(
            buf, sizeof(buf), "Block cache: %s used of %s, %" PRIu64 " hits, %" PRIu64 " misses\n",
            human_bytes(table_cache_->block_cache() ? table_cache_->block_cache()->total_charge() : 0).c_str(),
            human_bytes(options_.block_cache_bytes).c_str(),
            table_cache_->block_cache() ? table_cache_->block_cache()->hits() : 0,
            table_cache_->block_cache() ? table_cache_->block_cache()->misses() : 0);
        *value += buf;
        return true;
    }

    if (name == "sstables") {
        *value = versions_->current()->debug_string();
        return true;
    }

    if (name == "approximate-memory-usage") {
        std::size_t total = mem_->approximate_memory_usage();
        if (imm_ != nullptr) total += imm_->approximate_memory_usage();
        if (table_cache_->block_cache() != nullptr) total += table_cache_->block_cache()->total_charge();
        *value = std::to_string(total);
        return true;
    }

    if (name == "last-sequence") {
        *value = std::to_string(versions_->last_sequence());
        return true;
    }

    if (name == "background-error") {
        *value = bg_error_.to_string();
        return true;
    }

    if (name == "num-snapshots") {
        *value = std::to_string(snapshots_.size());
        return true;
    }

    if (name == "levels") {
        *value = versions_->level_summary();
        return true;
    }

    return false;
}

void DBImpl::get_approximate_sizes(std::span<const Range> ranges, std::span<std::uint64_t> sizes) {
    std::shared_ptr<Version> v;
    {
        Lock lock(mutex_);
        v = versions_->current();
    }
    for (std::size_t i = 0; i < ranges.size() && i < sizes.size(); ++i) {
        const InternalKey start(ranges[i].start, kMaxSequenceNumber, kValueTypeForSeek);
        const InternalKey limit(ranges[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
        const std::uint64_t a = versions_->approximate_offset_of(*v, start);
        const std::uint64_t b = versions_->approximate_offset_of(*v, limit);
        sizes[i] = b >= a ? b - a : 0;
    }
}

// ---------------------------------------------------------------------------

Status destroy_db(const std::string& path, const Options& options) {
    Env* env = options.env != nullptr ? options.env : Env::default_env();
    std::vector<std::string> names;
    Status s = env->get_children(path, &names);
    if (!s.is_ok()) return Status::ok();  // nothing to destroy

    std::unique_ptr<FileLock> lock;
    const std::string lockname = lock_file_name(path);
    s = env->lock_file(lockname, &lock);
    if (!s.is_ok()) return s;

    for (const auto& name : names) {
        std::uint64_t number = 0;
        FileType type;
        if (parse_file_name(name, &number, &type) && type != FileType::DbLock) {
            Status del = env->remove_file(path + "/" + name);
            if (s.is_ok() && !del.is_ok()) s = del;
        }
    }
    (void)env->unlock_file(std::move(lock));
    (void)env->remove_file(lockname);
    (void)env->remove_dir(path);  // fails if other files remain: leave them
    return s;
}

}  // namespace strata
