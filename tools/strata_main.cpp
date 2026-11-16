// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Entry point for the `strata` command-line tool; see cli.cpp.

#include <iostream>
#include <string>
#include <vector>

#include "cli.hpp"

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return strata::cli::run(args, std::cin, std::cout, std::cerr);
}
