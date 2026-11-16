// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// The `strata` command-line tool, as a function the tests can call.

#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace strata {
class Env;
}

namespace strata::cli {

/// Runs one command. args excludes the program name. Returns the process
/// exit code: 0 success, 1 failure (including "not found"), 2 bad usage.
/// `env` null means the local file system.
int run(const std::vector<std::string>& args, std::istream& in, std::ostream& out, std::ostream& err,
        Env* env = nullptr);

/// Splits a shell command line into words. Double quotes group words;
/// inside them \" is a quote. Returns false on an unterminated quote.
bool split_words(const std::string& line, std::vector<std::string>* words);

}  // namespace strata::cli
