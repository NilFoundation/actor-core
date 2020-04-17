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

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <boost/intrusive_ptr.hpp>

#include <nil/actor/detail/append_hex.hpp>
#include <nil/actor/detail/comparable.hpp>
#include <nil/actor/detail/type_traits.hpp>

namespace nil::actor {

    /// An intrusive, reference counting smart pointer implementation.
    /// @relates ref_counted
    template<class T>
    using intrusive_ptr = boost::intrusive_ptr<T>;

    template<typename T>
    struct has_weak_ptr_semantics {
        constexpr static const bool value = false;
    };

    template<typename T>
    struct has_weak_ptr_semantics<intrusive_ptr<T>> {
        constexpr static const bool value = false;
    };

    template<class T>
    std::string to_string(const intrusive_ptr<T> &x) {
        std::string result;
        detail::append_hex(result, reinterpret_cast<uintptr_t>(x.get()));
        return result;
    }
}    // namespace nil::actor
