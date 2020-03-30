//---------------------------------------------------------------------------//
// Copyright (c) 2011-2018 Dominik Charousset
// Copyright (c) 2017-2020 Mikhail Komarov <nemo@nil.foundation>
//
// Distributed under the terms and conditions of the BSD 3-Clause License or
// (at your option) under the terms and conditions of the Boost Software
// License 1.0. See accompanying files LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.
//---------------------------------------------------------------------------//

#define BOOST_TEST_MODULE logger

#include <nil/actor/logger.hpp>

#include "core_test.hpp"

#include <ctime>
#include <string>

#include <nil/actor/all.hpp>

using namespace nil::actor;
using namespace std::chrono;

using std::string;

#define CHECK_FUN_PREFIX(PrefixName)                                                                         \
    do {                                                                                                     \
        auto e = ACTOR_LOG_MAKE_EVENT(0, "caf", ACTOR_LOG_LEVEL_DEBUG, "");                                      \
        std::ostringstream oss;                                                                              \
        ::nil::actor::logger::render_fun_prefix(oss, e);                                                            \
        auto prefix = oss.str();                                                                             \
        if (prefix != PrefixName)                                                                            \
            BOOST_ERROR("rendering the prefix of " << e.pretty_fun << " produced " << prefix << " instead of " \
                                                 << PrefixName);                                             \
        else                                                                                                 \
            BOOST_CHECK_EQUAL(prefix, PrefixName);                                                             \
    } while (false)

void global_fun() {
    CHECK_FUN_PREFIX("GLOBAL");
}

// Little helper that allows us to write a single check for all compilers.
// Clang expands template parameters in __PRETTY_FUNCTION__, while GCC does
// not. For example, Clang would produce "void foo<int>::bar()", while GCC
// would produce "void foo<T>::bar() [with T = int]". A type called T gives
// us always the same output for the prefix.
struct T {};

namespace {

    struct fixture {
        fixture() {
            cfg.set("scheduler.policy", "testing");
        }

        void add(logger::field_type kind) {
            lf.emplace_back(logger::field {kind, std::string {}});
        }

        template<size_t N>
        void add(logger::field_type kind, const char (&str)[N]) {
            lf.emplace_back(logger::field {kind, std::string {str, str + (N - 1)}});
        }

        template<class F, class... Ts>
        string render(F f, Ts &&... xs) {
            std::ostringstream oss;
            f(oss, std::forward<Ts>(xs)...);
            return oss.str();
        }

        spawner_config cfg;
        logger::line_format lf;
    };

    void fun() {
        CHECK_FUN_PREFIX("$");
    }

    const char *ptr_fun(const char *x) {
        CHECK_FUN_PREFIX("$");
        return x;
    }

    const int &ref_fun(const int &x) {
        CHECK_FUN_PREFIX("$");
        return x;
    }

    template<class T>
    struct tpl {
        static void run() {
            CHECK_FUN_PREFIX("$.tpl<T>");
        }
    };

    namespace foo {

        void fun() {
            CHECK_FUN_PREFIX("$.foo");
        }

        const char *ptr_fun(const char *x) {
            CHECK_FUN_PREFIX("$.foo");
            return x;
        }

        const int &ref_fun(const int &x) {
            CHECK_FUN_PREFIX("$.foo");
            return x;
        }

        template<class T>
        struct tpl {
            static void run() {
                CHECK_FUN_PREFIX("$.foo.tpl<T>");
            }
        };

    }    // namespace foo

    constexpr const char *file_format = "%r %c %p %a %t %C %M %F:%L %m%n";

}    // namespace

BOOST_FIXTURE_TEST_SUITE(logger_tests, fixture)

// copy construction, copy assign, move construction, move assign
// and finally serialization round-trip
BOOST_AUTO_TEST_CASE(parse_default_format_strings) {
    spawner sys {cfg};
    add(logger::runtime_field);
    add(logger::plain_text_field, " ");
    add(logger::category_field);
    add(logger::plain_text_field, " ");
    add(logger::priority_field);
    add(logger::plain_text_field, " ");
    add(logger::actor_field);
    add(logger::plain_text_field, " ");
    add(logger::thread_field);
    add(logger::plain_text_field, " ");
    add(logger::class_name_field);
    add(logger::plain_text_field, " ");
    add(logger::method_field);
    add(logger::plain_text_field, " ");
    add(logger::file_field);
    add(logger::plain_text_field, ":");
    add(logger::line_field);
    add(logger::plain_text_field, " ");
    add(logger::message_field);
    add(logger::newline_field);
    BOOST_CHECK_EQUAL(logger::parse_format(file_format), lf);
#if ACTOR_LOG_LEVEL >= 0
    // Not parsed when compiling without logging enabled.
    BOOST_CHECK_EQUAL(sys.logger().file_format(), lf);
#endif
}

BOOST_AUTO_TEST_CASE(rendering) {
    // Rendering of time points.
    timestamp t0;
    time_t t0_t = 0;
    char t0_buf[50];
    BOOST_REQUIRE(strftime(t0_buf, sizeof(t0_buf), "%Y-%m-%dT%H:%M:%S.000", localtime(&t0_t)));
    BOOST_CHECK_EQUAL(render(logger::render_date, t0), t0_buf);
    // Rendering of events.
    logger::event e {
        ACTOR_LOG_LEVEL_WARNING,      42, "unit_test", "void ns::foo::bar()", "bar", "foo.cpp", "hello world",
        std::this_thread::get_id(), 0,  t0,
    };
    BOOST_CHECK_EQUAL(render(logger::render_fun_name, e), string_view {"bar"});
    BOOST_CHECK_EQUAL(render(logger::render_fun_prefix, e), string_view {"ns.foo"});
    // Exclude %r and %t from rendering test because they are nondeterministic.
    spawner sys {cfg};
    auto lf = logger::parse_format("%c %p %a %C %M %F:%L %m");
    auto &lg = sys.logger();
    using namespace std::placeholders;
    auto render_event = bind(&logger::render, &lg, _1, _2, _3);
    BOOST_CHECK_EQUAL(render(render_event, lf, e), "unit_test WARN actor0 ns.foo bar foo.cpp:42 hello world");
}

BOOST_AUTO_TEST_CASE(render_fun_prefix) {
    int i = 42;
    global_fun();
    fun();
    ptr_fun(nullptr);
    ref_fun(i);
    tpl<T>::run();
    foo::fun();
    foo::ptr_fun(nullptr);
    foo::ref_fun(i);
    foo::tpl<T>::run();
}

BOOST_AUTO_TEST_SUITE_END()
