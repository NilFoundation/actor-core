//---------------------------------------------------------------------------//
// Copyright (c) 2011-2018 Dominik Charousset
// Copyright (c) 2017-2020 Mikhail Komarov <nemo@nil.foundation>
//
// Distributed under the terms and conditions of the BSD 3-Clause License or
// (at your option) under the terms and conditions of the Boost Software
// License 1.0. See accompanying files LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.
//---------------------------------------------------------------------------//

#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

#include <nil/actor/abstract_actor.hpp>
#include <nil/actor/actor_control_block.hpp>
#include <nil/actor/config.hpp>

#ifdef BOOST_COMP_GNUC_AVAILABLE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

namespace nil {
    namespace actor {

        template<class T>
        class actor_storage {
        public:
            template<class... Us>
            actor_storage(actor_id x, node_id y, spawner *sys, Us &&... zs) : ctrl(x, y, sys, data_dtor, block_dtor) {
                // construct data member
                new (&data) T(std::forward<Us>(zs)...);
            }

            ~actor_storage() {
                // 1) make sure control block fits into a single cache line
                static_assert(sizeof(actor_control_block) < ACTOR_CACHE_LINE_SIZE,
                              "actor_control_block exceeds a single cache line");
// Clang in combination with libc++ on Linux complains about offsetof:
//     error: 'actor_storage' does not refer to a value
// Until we have found a reliable solution, we disable this safety check.
#if !(defined(BOOST_COMP_CLANG_AVAILABLE) && defined(BOOST_OS_LINUX_AVAILABLE))
                // 2) make sure reinterpret cast of the control block to the storage works
                static_assert(offsetof(actor_storage, ctrl) == 0, "control block is not at the start of the storage");
                // 3) make sure we can obtain a data pointer by jumping one cache line
                static_assert(offsetof(actor_storage, data) == ACTOR_CACHE_LINE_SIZE,
                              "data is not at cache line size boundary");
#else
                // 4) make sure static_cast and reinterpret_cast
                //    between T* and abstract_actor* are identical
                constexpr abstract_actor *dummy = nullptr;
                constexpr T *derived_dummy = static_cast<T *>(dummy);
                static_assert(derived_dummy == nullptr,
                              "actor subtype has illegal memory alignment "
                              "(probably due to virtual inheritance)");
#endif
            }

            actor_storage(const actor_storage &) = delete;
            actor_storage &operator=(const actor_storage &) = delete;

            static_assert(sizeof(actor_control_block) < ACTOR_CACHE_LINE_SIZE, "actor_control_block exceeds 64 bytes");

            actor_control_block ctrl;
            char pad[ACTOR_CACHE_LINE_SIZE - sizeof(actor_control_block)];
            union {
                T data;
            };

        private:
            static void data_dtor(abstract_actor *ptr) {
                // safe due to static assert #3
                ptr->on_destroy();
                static_cast<T *>(ptr)->~T();
            }

            static void block_dtor(actor_control_block *ptr) {
                // safe due to static assert #2
                delete reinterpret_cast<actor_storage *>(ptr);
            }
        };

    }    // namespace actor
}    // namespace nil

#ifdef BOOST_COMP_GNUC_AVAILABLE
#pragma GCC diagnostic pop
#endif