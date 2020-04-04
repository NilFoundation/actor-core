//---------------------------------------------------------------------------//
// Copyright (c) 2011-2019 Dominik Charousset
// Copyright (c) 2017-2020 Mikhail Komarov <nemo@nil.foundation>
//
// Distributed under the terms and conditions of the BSD 3-Clause License or
// (at your option) under the terms and conditions of the Boost Software
// License 1.0. See accompanying files LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.
//---------------------------------------------------------------------------//

#define BOOST_TEST_MODULE spawner_config

#include <nil/actor/spawner_config.hpp>

#include <nil/actor/test/dsl.hpp>

#include <deque>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace nil::actor;

using namespace std::string_literals;

namespace boost {
    namespace test_tools {
        namespace tt_detail {
            template<template<typename...> class P, typename... T>
            struct print_log_value<P<T...>> {
                void operator()(std::ostream &, P<T...> const &) {
                }
            };

            template<template<typename, std::size_t> class P, typename T, std::size_t S>
            struct print_log_value<P<T, S>> {
                void operator()(std::ostream &, P<T, S> const &) {
                }
            };
            template<>
            struct print_log_value<uri> {
                void operator()(std::ostream &, uri const &) {
                }
            };
        }    // namespace tt_detail
    }        // namespace test_tools
}    // namespace boost

namespace {

    timespan operator"" _ms(unsigned long long x) {
        return std::chrono::duration_cast<timespan>(std::chrono::milliseconds(x));
    }

    uri operator"" _u(const char *str, size_t size) {
        return unbox(make_uri(string_view {str, size}));
    }

    using string_list = std::vector<std::string>;

    struct config : spawner_config {
        config_option_adder options(string_view category) {
            return opt_group {custom_options_, category};
        }

        void clear() {
            content.clear();
            remainder.clear();
        }
    };

    struct fixture {
        config cfg;

        config_option_adder options(string_view category) {
            return cfg.options(category);
        }

        void parse(const char *file_content, string_list args = {}) {
            cfg.clear();
            cfg.remainder.clear();
            std::istringstream ini {file_content};
            if (auto err = cfg.parse(std::move(args), ini))
                BOOST_FAIL("parse() failed: " << cfg.render(err));
        }
    };

}    // namespace

BOOST_FIXTURE_TEST_SUITE(spawner_config_tests, fixture)

BOOST_AUTO_TEST_CASE(parsing_without_cli_arguments) {
    auto text = "[foo]\nbar=\"hello\"";
    options("?foo").add<std::string>("bar,b", "some string parameter");
    parse(text);
    BOOST_CHECK(cfg.remainder.empty());
    BOOST_CHECK_EQUAL(get_or(cfg, "foo.bar", ""), "hello");
}

BOOST_AUTO_TEST_CASE(parsing_without_cli_cfg_remainder) {
    auto text = "[foo]\nbar=\"hello\"";
    options("?foo").add<std::string>("bar,b", "some string parameter");
    BOOST_TEST_MESSAGE("CLI long name");
    parse(text, {"--foo.bar=test"});
    BOOST_CHECK(cfg.remainder.empty());
    BOOST_CHECK_EQUAL(get_or(cfg, "foo.bar", ""), "test");
    BOOST_TEST_MESSAGE("CLI abbreviated long name");
    parse(text, {"--bar=test"});
    BOOST_CHECK(cfg.remainder.empty());
    BOOST_CHECK_EQUAL(get_or(cfg, "foo.bar", ""), "test");
    BOOST_TEST_MESSAGE("CLI short name");
    parse(text, {"-b", "test"});
    BOOST_CHECK(cfg.remainder.empty());
    BOOST_CHECK_EQUAL(get_or(cfg, "foo.bar", ""), "test");
    BOOST_TEST_MESSAGE("CLI short name without whitespace");
    parse(text, {"-btest"});
    BOOST_CHECK(cfg.remainder.empty());
    BOOST_CHECK_EQUAL(get_or(cfg, "foo.bar", ""), "test");
}

BOOST_AUTO_TEST_CASE(parsing_with_cli_cfg_remainder) {
    auto text = "[foo]\nbar=\"hello\"";
    options("?foo").add<std::string>("bar,b", "some string parameter");
    BOOST_TEST_MESSAGE("valid cfg.remainder");
    parse(text, {"-b", "test", "hello", "world"});
    BOOST_CHECK_EQUAL(get_or(cfg, "foo.bar", ""), "test");
    BOOST_CHECK_EQUAL(cfg.remainder, string_list({"hello", "world"}));
    BOOST_TEST_MESSAGE("invalid cfg.remainder");
}

// Checks whether both a synced variable and the corresponding entry in
// content(cfg) are equal to `value`.
#define CHECK_SYNCED(var, value)                               \
    do {                                                       \
        BOOST_CHECK_EQUAL(var, value);                           \
        BOOST_CHECK_EQUAL(get<decltype(var)>(cfg, #var), value); \
    } while (false)

// Checks whether an entry in content(cfg) is equal to `value`.
#define CHECK_TEXT_ONLY(type, var, value) BOOST_CHECK_EQUAL(get<type>(cfg, #var), value)

#define ADD(var) add(var, #var, "...")

#define VAR(type)               \
    auto some_##type = type {}; \
    options("global").add(some_##type, "some_" #type, "...")

#define NAMED_VAR(type, name) \
    auto name = type {};      \
    options("global").add(name, #name, "...")

BOOST_AUTO_TEST_CASE(integers_and_integer_containers_options) {
    // Use a wild mess of "list-like" and "map-like" containers from the STL.
    using int_list = std::vector<int>;
    using int_list_list = std::list<std::deque<int>>;
    using int_map = std::unordered_map<std::string, int>;
    using int_list_map = std::map<std::string, std::unordered_set<int>>;
    using int_map_list = std::set<std::map<std::string, int>>;
    auto text = R"__(
    some_int = 42
    yet_another_int = 123
    some_int_list = [1, 2, 3]
    some_int_list_list = [[1, 2, 3], [4, 5, 6]]
    some_int_map = {a = 1, b = 2, c = 3}
    some_int_list_map = {a = [1, 2, 3], b = [4, 5, 6]}
    some_int_map_list = [{a = 1, b = 2, c = 3}, {d = 4, e = 5, f = 6}]
  )__";
    NAMED_VAR(int, some_other_int);
    VAR(int);
    VAR(int_list);
    VAR(int_list_list);
    VAR(int_map);
    VAR(int_list_map);
    VAR(int_map_list);
    parse(text, {"--some_other_int=23"});
    CHECK_SYNCED(some_int, 42);
    CHECK_SYNCED(some_other_int, 23);
    CHECK_TEXT_ONLY(int, yet_another_int, 123);
    CHECK_SYNCED(some_int_list, int_list({1, 2, 3}));
    CHECK_SYNCED(some_int_list_list, int_list_list({{1, 2, 3}, {4, 5, 6}}));
    CHECK_SYNCED(some_int_map, int_map({{{"a", 1}, {"b", 2}, {"c", 3}}}));
    CHECK_SYNCED(some_int_list_map, int_list_map({{{"a", {1, 2, 3}}, {"b", {4, 5, 6}}}}));
    CHECK_SYNCED(some_int_map_list, int_map_list({{{"a", 1}, {"b", 2}, {"c", 3}}, {{"d", 4}, {"e", 5}, {"f", 6}}}));
}

BOOST_AUTO_TEST_CASE(basic_and_basic_containers_options) {
    using std::map;
    using std::string;
    using std::vector;
    using int_list = vector<int>;
    using bool_list = vector<bool>;
    using double_list = vector<double>;
    using timespan_list = vector<timespan>;
    using uri_list = vector<uri>;
    using string_list = vector<string>;
    using int_map = map<string, int>;
    using bool_map = map<string, bool>;
    using double_map = map<string, double>;
    using timespan_map = map<string, timespan>;
    using uri_map = map<string, uri>;
    using string_map = map<string, string>;
    auto text = R"__(
    some_int = 42
    some_bool = true
    some_double = 1e23
    some_timespan = 123ms
    some_uri = <foo:bar>
    some_string = "string"
    some_int_list = [1, 2, 3]
    some_bool_list = [false, true]
    some_double_list = [1., 2., 3.]
    some_timespan_list = [123ms, 234ms, 345ms]
    some_uri_list = [<foo:a>, <foo:b>, <foo:c>]
    some_string_list = ["a", "b", "c"]
    some_int_map = {a = 1, b = 2, c = 3}
    some_bool_map = {a = true, b = false}
    some_double_map = {a = 1., b = 2., c = 3.}
    some_timespan_map = {a = 123ms, b = 234ms, c = 345ms}
    some_uri_map = {a = <foo:a>, b = <foo:b>, c = <foo:c>}
    some_string_map = {a = "1", b = "2", c = "3"}
  )__";
    VAR(int);
    VAR(bool);
    VAR(double);
    VAR(timespan);
    VAR(uri);
    VAR(string);
    VAR(int_list);
    VAR(bool_list);
    VAR(double_list);
    VAR(timespan_list);
    VAR(uri_list);
    VAR(string_list);
    VAR(int_map);
    VAR(bool_map);
    VAR(double_map);
    VAR(timespan_map);
    VAR(uri_map);
    VAR(string_map);
    parse(text);
    BOOST_TEST_MESSAGE("check primitive types");
    CHECK_SYNCED(some_int, 42);
    CHECK_SYNCED(some_bool, true);
    CHECK_SYNCED(some_double, 1e23);
    CHECK_SYNCED(some_timespan, 123_ms);
    CHECK_SYNCED(some_uri, "foo:bar"_u);
    CHECK_SYNCED(some_string, "string"s);
    BOOST_TEST_MESSAGE("check list types");
    CHECK_SYNCED(some_int_list, int_list({1, 2, 3}));
    CHECK_SYNCED(some_bool_list, bool_list({false, true}));
    CHECK_SYNCED(some_double_list, double_list({1., 2., 3.}));
    CHECK_SYNCED(some_timespan_list, timespan_list({123_ms, 234_ms, 345_ms}));
    CHECK_SYNCED(some_uri_list, uri_list({"foo:a"_u, "foo:b"_u, "foo:c"_u}));
    CHECK_SYNCED(some_string_list, string_list({"a", "b", "c"}));
    BOOST_TEST_MESSAGE("check dictionary types");
    CHECK_SYNCED(some_int_map, int_map({{"a", 1}, {"b", 2}, {"c", 3}}));
    CHECK_SYNCED(some_bool_map, bool_map({{"a", true}, {"b", false}}));
    CHECK_SYNCED(some_double_map, double_map({{"a", 1.}, {"b", 2.}, {"c", 3.}}));
    CHECK_SYNCED(some_timespan_map, timespan_map({{"a", 123_ms}, {"b", 234_ms}, {"c", 345_ms}}));
    CHECK_SYNCED(some_uri_map, uri_map({{"a", "foo:a"_u}, {"b", "foo:b"_u}, {"c", "foo:c"_u}}));
    CHECK_SYNCED(some_string_map, string_map({{"a", "1"}, {"b", "2"}, {"c", "3"}}));
}

BOOST_AUTO_TEST_SUITE_END()