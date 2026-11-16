// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Human-readable dumps of strata's on-disk files, for debugging and for the
// `strata dump` command. The file type is worked out from the file name:
// NNNNNN.sst (table), NNNNNN.log (write-ahead log), MANIFEST-NNNNNN.

#pragma once

#include <ostream>
#include <string>

#include "strata/env.hpp"
#include "strata/status.hpp"

namespace strata {

struct DumpOptions {
    /// Print each key/value (tables, logs). Off: only summaries.
    bool show_entries = true;
    /// Print table properties and block layout.
    bool show_layout = true;
    /// Stop printing entries after this many (0 = no limit).
    std::uint64_t max_entries = 0;
    /// Print values as hex instead of escaped text.
    bool hex = false;
};

Status dump_file(Env* env, const std::string& path, const DumpOptions& options, std::ostream& out);

Status dump_table(Env* env, const std::string& path, const DumpOptions& options, std::ostream& out);
Status dump_log(Env* env, const std::string& path, const DumpOptions& options, std::ostream& out);
Status dump_manifest(Env* env, const std::string& path, const DumpOptions& options, std::ostream& out);

/// Reads every block of every live table file in the database at `path`,
/// verifying checksums and key order. Writes a line per problem to `out`.
Status check_db(const std::string& path, Env* env, std::ostream& out);

}  // namespace strata
