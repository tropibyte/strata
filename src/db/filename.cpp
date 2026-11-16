// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "db/filename.hpp"

#include <cstdio>

#include "util/strutil.hpp"

namespace strata {
namespace {

std::string numbered(const std::string& dbname, std::uint64_t number, const char* suffix) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/%06llu.%s", static_cast<unsigned long long>(number), suffix);
    return dbname + buf;
}

}  // namespace

std::string log_file_name(const std::string& dbname, std::uint64_t number) {
    return numbered(dbname, number, "log");
}

std::string table_file_name(const std::string& dbname, std::uint64_t number) {
    return numbered(dbname, number, "sst");
}

std::string temp_file_name(const std::string& dbname, std::uint64_t number) {
    return numbered(dbname, number, "dbtmp");
}

std::string descriptor_file_name(const std::string& dbname, std::uint64_t number) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu", static_cast<unsigned long long>(number));
    return dbname + buf;
}

std::string current_file_name(const std::string& dbname) {
    return dbname + "/CURRENT";
}
std::string lock_file_name(const std::string& dbname) {
    return dbname + "/LOCK";
}
std::string info_log_file_name(const std::string& dbname) {
    return dbname + "/LOG";
}
std::string old_info_log_file_name(const std::string& dbname) {
    return dbname + "/LOG.old";
}

bool parse_file_name(std::string_view name, std::uint64_t* number, FileType* type) {
    if (name == "CURRENT") {
        *number = 0;
        *type = FileType::Current;
        return true;
    }
    if (name == "LOCK") {
        *number = 0;
        *type = FileType::DbLock;
        return true;
    }
    if (name == "LOG" || name == "LOG.old") {
        *number = 0;
        *type = FileType::InfoLog;
        return true;
    }
    if (name.substr(0, 9) == "MANIFEST-") {
        std::string_view rest = name.substr(9);
        std::uint64_t num = 0;
        if (!consume_decimal(rest, &num) || !rest.empty()) return false;
        *number = num;
        *type = FileType::Descriptor;
        return true;
    }
    std::string_view rest = name;
    std::uint64_t num = 0;
    if (!consume_decimal(rest, &num)) return false;
    if (rest == ".log") {
        *type = FileType::Log;
    } else if (rest == ".sst") {
        *type = FileType::Table;
    } else if (rest == ".dbtmp") {
        *type = FileType::Temp;
    } else {
        return false;
    }
    *number = num;
    return true;
}

Status set_current_file(Env* env, const std::string& dbname, std::uint64_t descriptor_number) {
    std::string manifest = descriptor_file_name(dbname, descriptor_number);
    // CURRENT holds the file name without the directory, plus a newline.
    std::string contents = manifest.substr(dbname.size() + 1) + "\n";
    const std::string tmp = temp_file_name(dbname, descriptor_number);
    Status s = write_string_to_file(env, contents, tmp, /*sync=*/true);
    if (s.is_ok()) s = env->rename_file(tmp, current_file_name(dbname));
    if (s.is_ok()) s = env->sync_dir(dbname);
    if (!s.is_ok()) (void)env->remove_file(tmp);
    return s;
}

}  // namespace strata
