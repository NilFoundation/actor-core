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

#include <nil/actor/abstract_actor.hpp>
#include <nil/actor/actor.hpp>
#include <nil/actor/actor_cast.hpp>
#include <nil/actor/spawner.hpp>
#include <nil/actor/composed_type.hpp>
#include <nil/actor/decorator/sequencer.hpp>
#include <nil/actor/detail/mpi_splice.hpp>
#include <nil/actor/intrusive_ptr.hpp>
#include <nil/actor/make_actor.hpp>
#include <nil/actor/replies_to.hpp>
#include <nil/actor/stateful_actor.hpp>
#include <nil/actor/typed_actor_view_base.hpp>
#include <nil/actor/typed_behavior.hpp>
#include <nil/actor/typed_response_promise.hpp>

namespace nil {
    namespace actor {

        template<class... Sigs>
        class typed_event_based_actor;

        namespace io {

            template<class... Sigs>
            class typed_broker;

        }    // namespace io

        /// Identifies a statically typed actor.
        /// @tparam Sigs Signature of this actor as `replies_to<...>::with<...>`
        ///              parameter pack.
        template<class... Sigs>
        class typed_actor : detail::comparable<typed_actor<Sigs...>>,
                            detail::comparable<typed_actor<Sigs...>, actor>,
                            detail::comparable<typed_actor<Sigs...>, actor_addr>,
                            detail::comparable<typed_actor<Sigs...>, strong_actor_ptr> {
        public:
            static_assert(sizeof...(Sigs) > 0, "Empty typed actor handle");

            // -- friend types that need access to private ctors
            friend class local_actor;

            template<class>
            friend class data_processor;

            template<class...>
            friend class typed_actor;

            // allow conversion via actor_cast
            template<class, class, int>
            friend class actor_cast_access;

            /// Creates a new `typed_actor` type by extending this one with `Es...`.
            template<class... Es>
            using extend = typed_actor<Sigs..., Es...>;

            /// Creates a new `typed_actor` type by extending this one with another
            /// `typed_actor`.
            template<class... Ts>
            using extend_with = typename detail::extend_with_helper<typed_actor, Ts...>::type;

            /// Identifies the behavior type actors of this kind use
            /// for their behavior stack.
            using behavior_type = typed_behavior<Sigs...>;

            /// Identifies pointers to instances of this kind of actor.
            using pointer = typed_event_based_actor<Sigs...> *;

            /// Allows a view to an actor implementing this messaging interface without
            /// knowledge of the actual type..
            using pointer_view = typed_actor_pointer<Sigs...>;

            /// Identifies the base class for this kind of actor.
            using base = typed_event_based_actor<Sigs...>;

            /// Identifies pointers to brokers implementing this interface.
            using broker_pointer = io::typed_broker<Sigs...> *;

            /// Identifies the base class of brokers implementing this interface.
            using broker_base = io::typed_broker<Sigs...>;

            /// Stores the template parameter pack.
            using signatures = detail::type_list<Sigs...>;

            /// Identifies the base class for this kind of actor with actor.
            template<class State>
            using stateful_base = stateful_actor<State, base>;

            /// Identifies the base class for this kind of actor with actor.
            template<class State>
            using stateful_pointer = stateful_actor<State, base> *;

            /// Identifies the broker_base class for this kind of actor with actor.
            template<class State>
            using stateful_broker_base = stateful_actor<State, broker_base>;

            /// Identifies the broker_base class for this kind of actor with actor.
            template<class State>
            using stateful_broker_pointer = stateful_actor<State, broker_base> *;

            typed_actor() = default;
            typed_actor(typed_actor &&) = default;
            typed_actor(const typed_actor &) = default;
            typed_actor &operator=(typed_actor &&) = default;
            typed_actor &operator=(const typed_actor &) = default;

            template<class... Ts>
            typed_actor(const typed_actor<Ts...> &other) : ptr_(other.ptr_) {
                static_assert(detail::tl_subset_of<signatures, detail::type_list<Ts...>>::value,
                              "Cannot assign incompatible handle");
            }

            // allow `handle_type{this}` for typed actors
            template<class T, class = detail::enable_if_t<actor_traits<T>::is_statically_typed>>
            typed_actor(T *ptr) : ptr_(ptr->ctrl()) {
                static_assert(detail::tl_subset_of<signatures, typename T::signatures>::value,
                              "Cannot assign T* to incompatible handle type");
                ACTOR_ASSERT(ptr != nullptr);
            }

            // Enable `handle_type{self}` for typed actor views.
            template<class T, class = std::enable_if_t<std::is_base_of<typed_actor_view_base, T>::value>>
            explicit typed_actor(T ptr) : ptr_(ptr.internal_ptr()) {
                static_assert(detail::tl_subset_of<signatures, typename T::signatures>::value,
                              "Cannot assign T to incompatible handle type");
            }

            template<class... Ts>
            typed_actor &operator=(const typed_actor<Ts...> &other) {
                static_assert(detail::tl_subset_of<signatures, detail::type_list<Ts...>>::value,
                              "Cannot assign incompatible handle");
                ptr_ = other.ptr_;
                return *this;
            }

            inline typed_actor &operator=(std::nullptr_t) {
                ptr_.reset();
                return *this;
            }

            /// Queries whether this actor handle is valid.
            inline explicit operator bool() const {
                return static_cast<bool>(ptr_);
            }

            /// Queries whether this actor handle is invalid.
            inline bool operator!() const {
                return !ptr_;
            }

            /// Queries the address of the stored actor.
            actor_addr address() const noexcept {
                return {ptr_.get(), true};
            }

            /// Returns the ID of this actor.
            actor_id id() const noexcept {
                return ptr_->id();
            }

            /// Returns the origin node of this actor.
            node_id node() const noexcept {
                return ptr_->node();
            }

            /// Returns the hosting actor system.
            inline spawner &home_system() const noexcept {
                return *ptr_->home_system;
            }

            /// Exchange content of `*this` and `other`.
            void swap(typed_actor &other) noexcept {
                ptr_.swap(other.ptr_);
            }

            /// @cond PRIVATE

            abstract_actor *operator->() const noexcept {
                return ptr_->get();
            }

            abstract_actor &operator*() const noexcept {
                return *ptr_->get();
            }

            intptr_t compare(const typed_actor &x) const noexcept {
                return actor_addr::compare(get(), x.get());
            }

            intptr_t compare(const actor &x) const noexcept {
                return actor_addr::compare(get(), actor_cast<actor_control_block *>(x));
            }

            intptr_t compare(const actor_addr &x) const noexcept {
                return actor_addr::compare(get(), actor_cast<actor_control_block *>(x));
            }

            intptr_t compare(const strong_actor_ptr &x) const noexcept {
                return actor_addr::compare(get(), actor_cast<actor_control_block *>(x));
            }

            typed_actor(actor_control_block *ptr, bool add_ref) : ptr_(ptr, add_ref) {
                // nop
            }

            friend inline std::string to_string(const typed_actor &x) {
                return to_string(x.ptr_);
            }

            friend inline void append_to_string(std::string &x, const typed_actor &y) {
                return append_to_string(x, y.ptr_);
            }

            template<class Inspector>
            friend typename Inspector::result_type inspect(Inspector &f, typed_actor &x) {
                return f(x.ptr_);
            }

            /// Releases the reference held by handle `x`. Using the
            /// handle after invalidating it is undefined behavior.
            friend void destroy(typed_actor &x) {
                x.ptr_.reset();
            }

            /// @endcond

        private:
            actor_control_block *get() const noexcept {
                return ptr_.get();
            }

            actor_control_block *release() noexcept {
                return ptr_.release();
            }

            typed_actor(actor_control_block *ptr) : ptr_(ptr) {
                // nop
            }

            strong_actor_ptr ptr_;
        };

        template<typename... T>
        struct has_weak_ptr_semantics<typed_actor<T...>> {
            constexpr static const bool value = false;
        };

        /// @relates typed_actor
        template<class... Xs, class... Ys>
        bool operator==(const typed_actor<Xs...> &x, const typed_actor<Ys...> &y) noexcept {
            return actor_addr::compare(actor_cast<actor_control_block *>(x), actor_cast<actor_control_block *>(y)) == 0;
        }

        /// @relates typed_actor
        template<class... Xs, class... Ys>
        bool operator!=(const typed_actor<Xs...> &x, const typed_actor<Ys...> &y) noexcept {
            return !(x == y);
        }

        /// @relates typed_actor
        template<class... Xs>
        bool operator==(const typed_actor<Xs...> &x, std::nullptr_t) noexcept {
            return actor_addr::compare(actor_cast<actor_control_block *>(x), nullptr) == 0;
        }

        /// @relates typed_actor
        template<class... Xs>
        bool operator==(std::nullptr_t, const typed_actor<Xs...> &x) noexcept {
            return actor_addr::compare(actor_cast<actor_control_block *>(x), nullptr) == 0;
        }

        /// @relates typed_actor
        template<class... Xs>
        bool operator!=(const typed_actor<Xs...> &x, std::nullptr_t) noexcept {
            return !(x == nullptr);
        }

        /// @relates typed_actor
        template<class... Xs>
        bool operator!=(std::nullptr_t, const typed_actor<Xs...> &x) noexcept {
            return !(x == nullptr);
        }

        /// Returns a new actor that implements the composition `f.g(x) = f(g(x))`.
        /// @relates typed_actor
        template<class... Xs, class... Ys>
        composed_type_t<detail::type_list<Xs...>, detail::type_list<Ys...>> operator*(typed_actor<Xs...> f,
                                                                                      typed_actor<Ys...> g) {
            using result = composed_type_t<detail::type_list<Xs...>, detail::type_list<Ys...>>;
            auto &sys = g->home_system();
            auto mts = sys.message_types(detail::type_list<result> {});
            return make_actor<decorator::sequencer, result>(sys.next_actor_id(), sys.node(), &sys,
                                                            actor_cast<strong_actor_ptr>(std::move(f)),
                                                            actor_cast<strong_actor_ptr>(std::move(g)), std::move(mts));
        }

    }    // namespace actor
}    // namespace nil

// allow typed_actor to be used in hash maps
namespace std {
    template<class... Sigs>
    struct hash<nil::actor::typed_actor<Sigs...>> {
        size_t operator()(const nil::actor::typed_actor<Sigs...> &ref) const {
            return ref ? static_cast<size_t>(ref->id()) : 0;
        }
    };
}    // namespace std
