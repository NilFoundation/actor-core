//---------------------------------------------------------------------------//
// Copyright (c) 2011-2019 Dominik Charousset
// Copyright (c) 2017-2020 Mikhail Komarov <nemo@nil.foundation>
//
// Distributed under the terms and conditions of the BSD 3-Clause License or
// (at your option) under the terms and conditions of the Boost Software
// License 1.0. See accompanying files LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.
//---------------------------------------------------------------------------//

#pragma once

#include <nil/actor/broadcast_downstream_manager.hpp>
#include <nil/actor/detail/stream_source_driver_impl.hpp>
#include <nil/actor/detail/stream_source_impl.hpp>
#include <nil/actor/fwd.hpp>
#include <nil/actor/policy/arg.hpp>
#include <nil/actor/stream_source.hpp>
#include <nil/actor/stream_source_driver.hpp>
#include <nil/actor/stream_source_trait.hpp>

namespace nil {
    namespace actor {

        /// Creates a new continuous stream source by instantiating the default source
        /// implementation with `Driver`. The returned manager is not connected to any
        /// slot and thus not stored by the actor automatically.
        /// @param self Points to the hosting actor.
        /// @param xs Parameter pack for constructing the driver.
        /// @returns The new `stream_manager`.
        template<class Driver, class... Ts>
        typename Driver::source_ptr_type attach_continuous_stream_source(scheduled_actor *self, Ts &&... xs) {
            using detail::make_stream_source;
            auto mgr = make_stream_source<Driver>(self, std::forward<Ts>(xs)...);
            mgr->continuous(true);
            return mgr;
        }

        /// Creates a new continuous stream source by instantiating the default source
        /// implementation with `Driver`. The returned manager is not connected to any
        /// slot and thus not stored by the actor automatically.
        /// @param self Points to the hosting actor.
        /// @param init Function object for initializing the state of the source.
        /// @param pull Generator function object for producing downstream messages.
        /// @param done Predicate returning `true` when generator is done.
        /// @param fin Cleanup handler.
        /// @returns The new `stream_manager`.
        template<class Init, class Pull, class Done, class Finalize = unit_t, class Trait = stream_source_trait_t<Pull>,
                 class DownstreamManager = broadcast_downstream_manager<typename Trait::output>>
        stream_source_ptr<DownstreamManager> attach_continuous_stream_source(scheduled_actor *self, Init init,
                                                                             Pull pull, Done done, Finalize fin = {},
                                                                             policy::arg<DownstreamManager> = {}) {
            using state_type = typename Trait::state;
            static_assert(std::is_same<void(state_type &), typename detail::get_callable_trait<Init>::fun_sig>::value,
                          "Expected signature `void (State&)` for init function");
            static_assert(
                std::is_same<bool(const state_type &), typename detail::get_callable_trait<Done>::fun_sig>::value,
                "Expected signature `bool (const State&)` "
                "for done predicate function");
            using driver = detail::stream_source_driver_impl<DownstreamManager, Pull, Done, Finalize>;
            return attach_continuous_stream_source<driver>(self, std::move(init), std::move(pull), std::move(done),
                                                           std::move(fin));
        }

    }    // namespace actor
}    // namespace nil