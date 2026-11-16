// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// A tour of the public API: open, write, read, batch, iterate, snapshot.
// Uses only the installed headers.
//
//   basic_usage [directory]     (default: ./strata-example)

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "strata/strata.hpp"

namespace {

void check(const strata::Status& s, const char* what) {
    if (!s.is_ok()) {
        std::cerr << what << ": " << s.to_string() << "\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "strata-example";

    strata::Options options;
    options.create_if_missing = true;
    std::unique_ptr<strata::DB> db;
    check(strata::DB::open(options, path, &db), "open");

    // Single writes.
    check(db->put({}, "city:atlanta", "GA"), "put");
    check(db->put({}, "city:boston", "MA"), "put");
    check(db->put({}, "city:chicago", "IL"), "put");

    std::string state;
    check(db->get({}, "city:boston", &state), "get");
    std::cout << "boston is in " << state << "\n";

    // A batch applies atomically.
    strata::WriteBatch batch;
    batch.remove("city:chicago");
    batch.put("city:denver", "CO");
    strata::WriteOptions durable;
    durable.sync = true;  // survive a machine crash, not just a process crash
    check(db->write(durable, &batch), "write");

    // A snapshot freezes a view.
    const strata::Snapshot* before = db->get_snapshot();
    check(db->put({}, "city:atlanta", "Georgia"), "put");

    strata::ReadOptions at_snapshot;
    at_snapshot.snapshot = before;
    check(db->get(at_snapshot, "city:atlanta", &state), "get");
    std::cout << "atlanta at the snapshot: " << state << "\n";
    db->release_snapshot(before);

    // Ordered iteration over a key range.
    auto it = db->new_iterator({});
    for (it->seek("city:"); it->valid() && it->key().substr(0, 5) == "city:"; it->next()) {
        std::cout << it->key() << " = " << it->value() << "\n";
    }
    check(it->status(), "iterate");
    it.reset();  // iterators must go before the database

    std::string levels;
    db->get_property("strata.levels", &levels);
    std::cout << "table files per level: " << levels << "\n";
    return 0;
}
