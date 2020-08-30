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

#include <utility>

namespace nil {
    namespace actor {
        namespace detail {

            template<class T>
            class consumer {
            public:
                using value_type = T;

                explicit consumer(T &x) : x_(x) {
                    // nop
                }

                void value(T &&y) {
                    x_ = std::move(y);
                }

            private:
                T &x_;
            };

            template<class T>
            consumer<T> make_consumer(T &x) {
                return consumer<T> {x};
            }

        }    // namespace detail
    }        // namespace actor
}    // namespace nil