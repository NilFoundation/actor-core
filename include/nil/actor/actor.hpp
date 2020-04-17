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

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include <nil/actor/abstract_actor.hpp>
#include <nil/actor/actor_control_block.hpp>
#include <nil/actor/actor_traits.hpp>
#include <nil/actor/config.hpp>
#include <nil/actor/detail/comparable.hpp>

#include <nil/actor/detail/type_traits.hpp>
#include <nil/actor/fwd.hpp>
#include <nil/actor/message.hpp>

namespace nil::actor {

    /// Identifies an untyped actor. Can be used with derived types
    /// of `event_based_actor`, `blocking_actor`, and `actor_proxy`.
    class BOOST_SYMBOL_VISIBLE actor : detail::comparable<actor>,
                                       detail::comparable<actor, actor_addr>,
                                       detail::comparable<actor, strong_actor_ptr> {
    public:
        // -- friend types that need access to private ctors
        friend class local_actor;

        using signatures = none_t;

        // allow conversion via actor_cast
        template<class, class, int>
        friend class actor_cast_access;

        actor() = default;
        actor(actor &&) = default;
        actor(const actor &) = default;
        actor &operator=(actor &&) = default;
        actor &operator=(const actor &) = default;

        actor(std::nullptr_t);

        actor(const scoped_actor &);

        template<class T, class = detail::enable_if_t<actor_traits<T>::is_dynamically_typed>>
        actor(T *ptr) : ptr_(ptr->ctrl()) {
            ACTOR_ASSERT(ptr != nullptr);
        }

        template<class T, class = detail::enable_if_t<actor_traits<T>::is_dynamically_typed>>
        actor &operator=(intrusive_ptr<T> ptr) {
            actor tmp {std::move(ptr)};
            swap(tmp);
            return *this;
        }

        template<class T, class = detail::enable_if_t<actor_traits<T>::is_dynamically_typed>>
        actor &operator=(T *ptr) {
            actor tmp {ptr};
            swap(tmp);
            return *this;
        }

        actor &operator=(std::nullptr_t);

        actor &operator=(const scoped_actor &x);

        /// Queries whether this actor handle is valid.
        inline explicit operator bool() const {
            return static_cast<bool>(ptr_);
        }

        /// Queries whether this actor handle is invalid.
        inline bool operator!() const {
            return !ptr_;
        }

        /// Returns the address of the stored actor.
        actor_addr address() const noexcept;

        /// Returns the ID of this actor.
        inline actor_id id() const noexcept {
            return ptr_->id();
        }

        /// Returns the origin node of this actor.
        inline node_id node() const noexcept {
            return ptr_->node();
        }

        /// Returns the hosting actor system.
        inline spawner &home_system() const noexcept {
            return *ptr_->home_system;
        }

        /// Exchange content of `*this` and `other`.
        void swap(actor &other) noexcept;

        /// @cond PRIVATE

        inline abstract_actor *operator->() const noexcept {
            ACTOR_ASSERT(ptr_);
            return ptr_->get();
        }

        intptr_t compare(const actor &) const noexcept;

        intptr_t compare(const actor_addr &) const noexcept;

        intptr_t compare(const strong_actor_ptr &) const noexcept;

        actor(actor_control_block *, bool);

        /// @endcond

        friend inline std::string to_string(const actor &x) {
            return to_string(x.ptr_);
        }

        friend inline void append_to_string(std::string &x, const actor &y) {
            return append_to_string(x, y.ptr_);
        }

        template<class Inspector>
        friend typename Inspector::result_type inspect(Inspector &f, actor &x) {
            return inspect(f, x.ptr_);
        }

        /// Releases the reference held by handle `x`. Using the
        /// handle after invalidating it is undefined behavior.
        friend void destroy(actor &x) {
            x.ptr_.reset();
        }

    private:
        inline actor_control_block *get() const noexcept {
            return ptr_.get();
        }

        inline actor_control_block *detach() noexcept {
            return ptr_.detach();
        }

        actor(actor_control_block *);

        strong_actor_ptr ptr_;
    };

    template<>
    struct has_weak_ptr_semantics<actor> {
        constexpr static const bool value = false;
    };

    /// Combine `f` and `g` so that `(f*g)(x) = f(g(x))`.
    BOOST_SYMBOL_VISIBLE actor operator*(actor f, actor g);

    /// @relates actor
    BOOST_SYMBOL_VISIBLE bool operator==(const actor &lhs, abstract_actor *rhs);

    /// @relates actor
    BOOST_SYMBOL_VISIBLE bool operator==(abstract_actor *lhs, const actor &rhs);

    /// @relates actor
    BOOST_SYMBOL_VISIBLE bool operator!=(const actor &lhs, abstract_actor *rhs);

    /// @relates actor
    BOOST_SYMBOL_VISIBLE bool operator!=(abstract_actor *lhs, const actor &rhs);

}    // namespace nil::actor

// allow actor to be used in hash maps
namespace std {
    template<>
    struct hash<nil::actor::actor> {
        inline size_t operator()(const nil::actor::actor &ref) const {
            return static_cast<size_t>(ref ? ref->id() : 0);
        }
    };
}    // namespace std