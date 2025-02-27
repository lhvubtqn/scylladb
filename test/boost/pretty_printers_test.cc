/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define BOOST_TEST_MODULE utils

#include <sstream>
#include <boost/test/unit_test.hpp>
#include "utils/pretty_printers.hh"

BOOST_AUTO_TEST_CASE(test_print_data_size) {
    struct {
        size_t n;
        std::string_view formatted;
    } sizes[] = {
        {0ULL, "0 bytes"},
        {42ULL, "42 bytes"},
        {10'000ULL, "10kB"},
        {10'000'000ULL, "10MB"},
        {10'000'000'000ULL, "10GB"},
        {10'000'000'000'000ULL, "10TB"},
        {10'000'000'000'000'000ULL, "10PB"},
        {10'000'000'000'000'000'000ULL, "10000PB"},
    };
    for (auto [n, expected] : sizes) {
        std::stringstream out;
        out << utils::pretty_printed_data_size{n};
        auto actual = out.str();
        BOOST_CHECK_EQUAL(actual, expected);
    }
}

BOOST_AUTO_TEST_CASE(test_print_throughput) {
    struct {
        size_t n;
        float seconds;
        std::string_view formatted;
    } sizes[] = {
        {0ULL, 0.F, "0 bytes/s"},
        {42ULL, 0.F, "0 bytes/s"},
        {42ULL, 1.F, "42 bytes/s"},
        {42ULL, 0.5F, "84 bytes/s"},
        {10'000'000ULL, 1.F, "10MB/s"},
        {10'000'000ULL, 0.5F, "20MB/s"},
    };
    for (auto [n, seconds, expected] : sizes) {
        std::stringstream out;
        out << utils::pretty_printed_throughput{n, std::chrono::duration<float>(seconds)};
        auto actual = out.str();
        BOOST_CHECK_EQUAL(actual, expected);
    }
}
