// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Names of the files in a database directory:
//     CURRENT            name of the live MANIFEST
//     LOCK               held while the database is open
//     LOG, LOG.old       informational log
//     MANIFEST-NNNNNN    the version log: which table files are in each level
//     NNNNNN.log         write-ahead log
//     NNNNNN.sst         table file
//     NNNNNN.dbtmp       temporary file (CURRENT is written through one)

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "strata/env.hpp"

namespace strata {

enum class FileType {
    Log,
    DbLock,
    Table,
    Descriptor,
    Current,
    Temp,
    InfoLog,
};

std::string log_file_name(const std::string& dbname, std::uint64_t number);
std::string table_file_name(const std::string& dbname, std::uint64_t number);
std::string descriptor_file_name(const std::string& dbname, std::uint64_t number);
std::string current_file_name(const std::string& dbname);
std::string lock_file_name(const std::string& dbname);
std::string temp_file_name(const std::string& dbname, std::uint64_t number);
std::string info_log_file_name(const std::string& dbname);
std::string old_info_log_file_name(const std::string& dbname);

/// Recognizes a file name (no directory). number is 0 for files without one.
bool parse_file_name(std::string_view name, std::uint64_t* number, FileType* type);

/// Points CURRENT at MANIFEST-<descriptor_number>, atomically (write a temp
/// file, then rename it over CURRENT).
Status set_current_file(Env* env, const std::string& dbname, std::uint64_t descriptor_number);

}  // namespace strata
