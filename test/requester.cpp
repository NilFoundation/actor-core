//---------------------------------------------------------------------------//
// Copyright (c) 2011-2018 Dominik Charousset
// Copyright (c) 2017-2020 Mikhail Komarov <nemo@nil.foundation>
//
// Distributed under the terms and conditions of the BSD 3-Clause License or
// (at your option) under the terms and conditions of the Boost Software
// License 1.0. See accompanying files LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.
//---------------------------------------------------------------------------//

#define BOOST_TEST_MODULE mixin.requester

#include <nil/actor/mixin/requester.hpp>

#include <nil/actor/test/dsl.hpp>

#include <numeric>

#include <nil/actor/event_based_actor.hpp>
#include <nil/actor/policy/select_all.hpp>

using namespace nil::actor;

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
            struct print_log_value<nil::actor::unit_t> {
                void operator()(std::ostream &, nil::actor::unit_t const &) {
                }
            };
        }    // namespace tt_detail
    }        // namespace test_tools
}    // namespace boost

namespace {

    using discarding_server_type = typed_actor<replies_to<int, int>::with<void>>;

    using adding_server_type = typed_actor<replies_to<int, int>::with<int>>;

    using result_type = variant<none_t, unit_t, int>;

    struct fixture : test_coordinator_fixture<> {
        fixture() {
            result = std::make_shared<result_type>(none);
            discarding_server = make_server([](int, int) {});
            adding_server = make_server([](int x, int y) { return x + y; });
            run();
        }

        template<class F>
        auto make_server(F f) -> typed_actor<replies_to<int, int>::with<decltype(f(1, 2))>> {
            using impl = typed_actor<replies_to<int, int>::with<decltype(f(1, 2))>>;
            auto init = [f]() -> typename impl::behavior_type {
                return {
                    [f](int x, int y) { return f(x, y); },
                };
            };
            return sys.spawn(init);
        }

        template<class T>
        T make_delegator(T dest) {
            auto f = [=](typename T::pointer self) -> typename T::behavior_type {
                return {
                    [=](int x, int y) { return self->delegate(dest, x, y); },
                };
            };
            return sys.spawn<lazy_init>(f);
        }

        std::shared_ptr<result_type> result = std::make_shared<result_type>(none);
        discarding_server_type discarding_server;
        adding_server_type adding_server;
    };

}    // namespace

#define ERROR_HANDLER [&](error &err) { BOOST_FAIL(sys.render(err)); }

#define SUBTEST(message)                     \
    *result = none;                          \
    run();                                   \
    BOOST_TEST_MESSAGE("subtest: " message); \
    for (int subtest_dummy = 0; subtest_dummy < 1; ++subtest_dummy)

BOOST_FIXTURE_TEST_SUITE(requester_tests, fixture)

BOOST_AUTO_TEST_CASE(requests_without_result) {
    auto server = discarding_server;
    SUBTEST("request.then") {
        auto client = sys.spawn(
            [=](event_based_actor *self) { self->request(server, infinite, 1, 2).then([=] { *result = unit; }); });
        run_once();
        expect((int, int), from(client).to(server).with(1, 2));
        expect((void), from(server).to(client));
        BOOST_CHECK_EQUAL(get<unit_t>(*result), unit);
    }
    SUBTEST("request.await") {
        auto client = sys.spawn(
            [=](event_based_actor *self) { self->request(server, infinite, 1, 2).await([=] { *result = unit; }); });
        run_once();
        expect((int, int), from(client).to(server).with(1, 2));
        expect((void), from(server).to(client));
        BOOST_CHECK_EQUAL(get<unit_t>(*result), unit);
    }
    SUBTEST("request.receive") {
        auto res_hdl = self->request(server, infinite, 1, 2);
        run();
        res_hdl.receive([&] { *result = unit; }, ERROR_HANDLER);
        BOOST_CHECK_EQUAL(get<unit_t>(*result), unit);
    }
}

BOOST_AUTO_TEST_CASE(requests_with_integer_result) {
    auto server = adding_server;
    SUBTEST("request.then") {
        auto client = sys.spawn(
            [=](event_based_actor *self) { self->request(server, infinite, 1, 2).then([=](int x) { *result = x; }); });
        run_once();
        expect((int, int), from(client).to(server).with(1, 2));
        expect((int), from(server).to(client).with(3));
        BOOST_CHECK_EQUAL(get<int>(*result), 3);
    }
    SUBTEST("request.await") {
        auto client = sys.spawn(
            [=](event_based_actor *self) { self->request(server, infinite, 1, 2).await([=](int x) { *result = x; }); });
        run_once();
        expect((int, int), from(client).to(server).with(1, 2));
        expect((int), from(server).to(client).with(3));
        BOOST_CHECK_EQUAL(get<int>(*result), 3);
    }
    SUBTEST("request.receive") {
        auto res_hdl = self->request(server, infinite, 1, 2);
        run();
        res_hdl.receive([&](int x) { *result = x; }, ERROR_HANDLER);
        BOOST_CHECK_EQUAL(get<int>(*result), 3);
    }
}

BOOST_AUTO_TEST_CASE(delegated_request_with_integer_result) {
    auto worker = adding_server;
    auto server = make_delegator(worker);
    auto client = sys.spawn(
        [=](event_based_actor *self) { self->request(server, infinite, 1, 2).then([=](int x) { *result = x; }); });
    run_once();
    expect((int, int), from(client).to(server).with(1, 2));
    expect((int, int), from(client).to(worker).with(1, 2));
    expect((int), from(worker).to(client).with(3));
    BOOST_CHECK_EQUAL(get<int>(*result), 3);
}

BOOST_AUTO_TEST_CASE(requesters_support_fan_out_request) {
    using policy::select_all;
    std::vector<adding_server_type> workers {
        make_server([](int x, int y) { return x + y; }),
        make_server([](int x, int y) { return x + y; }),
        make_server([](int x, int y) { return x + y; }),
    };
    run();
    auto sum = std::make_shared<int>(0);
    auto client = sys.spawn([=](event_based_actor *self) {
        self->fan_out_request<select_all>(workers, infinite, 1, 2).then([=](std::vector<int> results) {
            for (auto result : results)
                BOOST_CHECK_EQUAL(result, 3);
            *sum = std::accumulate(results.begin(), results.end(), 0);
        });
    });
    run_once();
    expect((int, int), from(client).to(workers[0]).with(1, 2));
    expect((int), from(workers[0]).to(client).with(3));
    expect((int, int), from(client).to(workers[1]).with(1, 2));
    expect((int), from(workers[1]).to(client).with(3));
    expect((int, int), from(client).to(workers[2]).with(1, 2));
    expect((int), from(workers[2]).to(client).with(3));
    BOOST_CHECK_EQUAL(*sum, 9);
}

#ifndef ACTOR_NO_EXCEPTIONS

BOOST_AUTO_TEST_CASE(exceptions_while_processing_requests_trigger_error_messages) {
    auto worker = sys.spawn([] {
        return behavior {
            [](int) { throw std::runtime_error(""); },
        };
    });
    run();
    auto client = sys.spawn([worker](event_based_actor *self) {
        self->request(worker, infinite, 42).then([](int) { BOOST_FAIL("unexpected handler called"); });
    });
    run_once();
    expect((int), from(client).to(worker).with(42));
    expect((error), from(worker).to(client).with(sec::runtime_error));
}

#endif    // ACTOR_NO_EXCEPTIONS

BOOST_AUTO_TEST_SUITE_END()