//---------------------------------------------------------------------------//
// Copyright (c) 2011-2020 Dominik Charousset
// Copyright (c) 2017-2020 Mikhail Komarov <nemo@nil.foundation>
//
// Distributed under the terms and conditions of the BSD 3-Clause License or
// (at your option) under the terms and conditions of the Boost Software
// License 1.0. See accompanying files LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.
//---------------------------------------------------------------------------//

#include <nil/actor/detail/meta_object.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <nil/actor/binary_deserializer.hpp>
#include <nil/actor/binary_serializer.hpp>
#include <nil/actor/config.hpp>
#include <nil/actor/deserializer.hpp>
#include <nil/actor/error.hpp>
#include <nil/actor/error_code.hpp>
#include <nil/actor/serializer.hpp>
#include <nil/actor/span.hpp>

namespace nil {
    namespace actor {
        namespace detail {

#define fatal(str)                           \
    do {                                     \
        fprintf(stderr, "FATAL: " str "\n"); \
        abort();                             \
    } while (false)

            namespace {

                // Stores global type information.
                meta_object *meta_objects;

                // Stores the size of `meta_objects`.
                size_t meta_objects_size;

                // Make sure to clean up all meta objects on program exit.
                struct meta_objects_cleanup {
                    ~meta_objects_cleanup() {
                        delete[] meta_objects;
                    }
                } cleanup_helper;

            }    // namespace

            nil::actor::error save(const meta_object &meta, nil::actor::serializer &sink, const void *obj) {
                return meta.save(sink, obj);
            }

            nil::actor::error_code<sec> save(const meta_object &meta, nil::actor::binary_serializer &sink,
                                             const void *obj) {
                return meta.save_binary(sink, obj);
            }

            nil::actor::error load(const meta_object &meta, nil::actor::deserializer &source, void *obj) {
                return meta.load(source, obj);
            }

            nil::actor::error_code<sec> load(const meta_object &meta, nil::actor::binary_deserializer &source,
                                             void *obj) {
                return meta.load_binary(source, obj);
            }

            span<const meta_object> global_meta_objects() {
                return {meta_objects, meta_objects_size};
            }

            const meta_object *global_meta_object(type_id_t id) {
                ACTOR_ASSERT(id < meta_objects_size);
                auto &meta = meta_objects[id];
                return meta.type_name != nullptr ? &meta : nullptr;
            }

            void clear_global_meta_objects() {
                if (meta_objects != nullptr) {
                    delete[] meta_objects;
                    meta_objects = nullptr;
                    meta_objects_size = 0;
                }
            }

            span<meta_object> resize_global_meta_objects(size_t size) {
                if (size <= meta_objects_size)
                    fatal(
                        "resize_global_meta_objects called with a new size that does not "
                        "grow the array");
                auto new_storage = new meta_object[size];
                std::copy(meta_objects, meta_objects + meta_objects_size, new_storage);
                delete[] meta_objects;
                meta_objects = new_storage;
                meta_objects_size = size;
                return {new_storage, size};
            }

            void set_global_meta_objects(type_id_t first_id, span<const meta_object> xs) {
                auto new_size = first_id + xs.size();
                if (first_id < meta_objects_size) {
                    if (new_size > meta_objects_size)
                        fatal(
                            "set_global_meta_objects called with "
                            "'first_id < meta_objects_size' and "
                            "'new_size > meta_objects_size'");
                    auto out = meta_objects + first_id;
                    for (const auto &x : xs) {
                        if (out->type_name == nullptr) {
                            // We support calling set_global_meta_objects for building the global
                            // table chunk-by-chunk.
                            *out = x;
                        } else if (strcmp(out->type_name, x.type_name) == 0) {
                            // nop: set_global_meta_objects implements idempotency.
                        } else {
                            fprintf(stderr,
                                    "FATAL: type ID %d already assigned to %s (tried to override "
                                    "with %s)\n",
                                    static_cast<int>(std::distance(meta_objects, out)), out->type_name, x.type_name);
                            abort();
                        }
                        ++out;
                    }
                    return;
                }
                auto dst = resize_global_meta_objects(first_id + xs.size());
                std::copy(xs.begin(), xs.end(), dst.begin() + first_id);
            }

        }    // namespace detail
    }        // namespace actor
}    // namespace nil