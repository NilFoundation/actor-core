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

#include <cstdint>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

#include <nil/actor/detail/squashed_int.hpp>
#include <nil/actor/detail/type_traits.hpp>
#include <nil/actor/error.hpp>
#include <nil/actor/fwd.hpp>
#include <nil/actor/message.hpp>
#include <nil/actor/none.hpp>
#include <nil/actor/parser_state.hpp>
#include <nil/actor/string_view.hpp>
#include <nil/actor/unit.hpp>

namespace nil {
    namespace actor {
        namespace detail {

            // -- boolean type -------------------------------------------------------------

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, bool &x);

            // -- signed integer types -----------------------------------------------------

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, int8_t &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, int16_t &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, int32_t &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, int64_t &x);

            // -- unsigned integer types ---------------------------------------------------

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, uint8_t &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, uint16_t &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, uint32_t &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, uint64_t &x);

            // -- non-fixed size integer types ---------------------------------------------

            template<class T>
            detail::enable_if_t<std::is_integral<T>::value> parse(string_parser_state &ps, T &x) {
                using squashed_type = squashed_int_t<T>;
                return parse(ps, reinterpret_cast<squashed_type &>(x));
            }

            // -- floating point types -----------------------------------------------------

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, float &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, double &x);

            // -- =nil; Actor types ----------------------------------------------------------------

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, timespan &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, ipv4_address &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, ipv4_subnet &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, ipv4_endpoint &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, ipv6_address &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, ipv6_subnet &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, ipv6_endpoint &x);

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, uri &x);

            // -- STL types ----------------------------------------------------------------

            BOOST_SYMBOL_VISIBLE void parse(string_parser_state &ps, std::string &x);

            // -- container types ----------------------------------------------------------

            BOOST_SYMBOL_VISIBLE void parse_element(string_parser_state &ps, std::string &x,
                                                    const char *char_blacklist);

            template<class T>
            enable_if_t<!is_pair<T>::value> parse_element(string_parser_state &ps, T &x, const char *);

            template<class First, class Second, size_t N>
            void parse_element(string_parser_state &ps, std::pair<First, Second> &kvp, const char (&char_blacklist)[N]);

            template<class T>
            enable_if_tt<is_iterable<T>> parse(string_parser_state &ps, T &xs) {
                using value_type = deconst_kvp_t<typename T::value_type>;
                static constexpr auto is_map_type = is_pair<value_type>::value;
                static constexpr auto opening_char = is_map_type ? '{' : '[';
                static constexpr auto closing_char = is_map_type ? '}' : ']';
                auto out = std::inserter(xs, xs.end());
                // List/map using [] or {} notation.
                if (ps.consume(opening_char)) {
                    char char_blacklist[] = {closing_char, ',', '\0'};
                    do {
                        if (ps.consume(closing_char)) {
                            ps.skip_whitespaces();
                            ps.code = ps.at_end() ? pec::success : pec::trailing_character;
                            return;
                        }
                        value_type tmp;
                        parse_element(ps, tmp, char_blacklist);
                        if (ps.code > pec::trailing_character)
                            return;
                        *out++ = std::move(tmp);
                    } while (ps.consume(','));
                    if (ps.consume(closing_char)) {
                        ps.skip_whitespaces();
                        ps.code = ps.at_end() ? pec::success : pec::trailing_character;
                    } else {
                        ps.code = pec::unexpected_character;
                    }
                    return;
                }
                // An empty string simply results in an empty list/map.
                if (ps.at_end())
                    return;
                // List/map without [] or {}.
                do {
                    char char_blacklist[] = {',', '\0'};
                    value_type tmp;
                    parse_element(ps, tmp, char_blacklist);
                    if (ps.code > pec::trailing_character)
                        return;
                    *out++ = std::move(tmp);
                } while (ps.consume(','));
                ps.code = ps.at_end() ? pec::success : pec::trailing_character;
            }

            template<class T>
            enable_if_t<!is_pair<T>::value> parse_element(string_parser_state &ps, T &x, const char *) {
                parse(ps, x);
            }

            template<class First, class Second, size_t N>
            void parse_element(string_parser_state &ps, std::pair<First, Second> &kvp,
                               const char (&char_blacklist)[N]) {
                static_assert(N > 0, "empty array");
                // TODO: consider to guard the blacklist computation with
                //       `if constexpr (is_same_v<First, string>)` when switching to C++17.
                char key_blacklist[N + 1];
                if (N > 1)
                    memcpy(key_blacklist, char_blacklist, N - 1);
                key_blacklist[N - 1] = '=';
                key_blacklist[N] = '\0';
                parse_element(ps, kvp.first, key_blacklist);
                if (ps.code > pec::trailing_character)
                    return;
                if (!ps.consume('=')) {
                    ps.code = pec::unexpected_character;
                    return;
                }
                parse_element(ps, kvp.second, char_blacklist);
            }

            // -- convenience functions ----------------------------------------------------

            template<class T>
            error parse(string_view str, T &x) {
                string_parser_state ps {str.begin(), str.end()};
                parse(ps, x);
                if (ps.code == pec::success)
                    return none;
                return make_error(ps.code, std::string {str.begin(), str.end()});
            }

        }    // namespace detail
    }        // namespace actor
}    // namespace nil