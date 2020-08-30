//---------------------------------------------------------------------------//
// Copyright (c) 2011-2020 Dominik Charousset
// Copyright (c) 2017-2020 Mikhail Komarov <nemo@nil.foundation>
//
// Distributed under the terms and conditions of the BSD 3-Clause License or
// (at your option) under the terms and conditions of the Boost Software
// License 1.0. See accompanying files LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.
//---------------------------------------------------------------------------//

#include <nil/actor/detail/type_id_list_builder.hpp>

#include <mutex>
#include <unordered_set>

#include <nil/actor/config.hpp>
#include <nil/actor/detail/fnv_hash.hpp>
#include <nil/actor/type_id_list.hpp>

namespace nil {
    namespace actor {
        namespace detail {
            namespace {

                struct dyn_type_id_list {
                    dyn_type_id_list(type_id_t *storage) : storage(storage) {
                        auto first = reinterpret_cast<unsigned char *>(storage);
                        auto last = first + ((storage[0] + 1) * sizeof(type_id_t));
                        hash = fnv_hash(first, last);
                    }

                    dyn_type_id_list(dyn_type_id_list &&other) : storage(other.storage), hash(other.hash) {
                        other.storage = nullptr;
                    }

                    ~dyn_type_id_list() {
                        free(storage);
                    }

                    type_id_t *storage;
                    size_t hash;
                };

                bool operator==(const dyn_type_id_list &x, const dyn_type_id_list &y) {
                    return type_id_list {x.storage} == type_id_list {y.storage};
                }

            }    // namespace
        }        // namespace detail
    }            // namespace actor
}    // namespace nil

namespace std {
    template<>
    struct hash<nil::actor::detail::dyn_type_id_list> {
        size_t operator()(const nil::actor::detail::dyn_type_id_list &x) const noexcept {
            return x.hash;
        }
    };
}    // namespace std

namespace nil {
    namespace actor {
        namespace detail {

            namespace {

                std::mutex type_id_list_cache_mx;
                std::unordered_set<dyn_type_id_list> type_id_list_cache;

                const type_id_t *get_or_set_type_id_buf(type_id_t *ptr) {
                    dyn_type_id_list dl {ptr};
                    std::unique_lock<std::mutex> guard {type_id_list_cache_mx};
                    auto iter = type_id_list_cache.emplace(std::move(dl)).first;
                    return iter->storage;
                }

            }    // namespace

            type_id_list_builder::type_id_list_builder() : size_(0), reserved_(0), storage_(nullptr) {
                // nop
            }

            type_id_list_builder::~type_id_list_builder() {
                free(storage_);
            }

            void type_id_list_builder::reserve(size_t new_capacity) {
                if (reserved_ >= new_capacity)
                    return;
                reserved_ = new_capacity;
                auto ptr = realloc(storage_, reserved_ * sizeof(type_id_t));
                if (ptr == nullptr)
                    throw std::bad_alloc();
                storage_ = reinterpret_cast<type_id_t *>(ptr);
                // Add the dummy for later inserting the size on first push_back.
                if (size_ == 0) {
                    storage_[0] = 0;
                    size_ = 1;
                }
            }

            void type_id_list_builder::push_back(type_id_t id) {
                if ((size_ + 1) >= reserved_)
                    reserve(reserved_ + block_size);
                storage_[size_++] = id;
            }

            size_t type_id_list_builder::size() const noexcept {
                // Index 0 is reserved for storing the (final) size, i.e., does not contain a
                // type ID.
                return size_ > 0 ? size_ - 1 : 0;
            }

            type_id_t type_id_list_builder::operator[](size_t index) const noexcept {
                ACTOR_ASSERT(index < size());
                return storage_[index + 1];
            }

            type_id_list type_id_list_builder::move_to_list() {
                if (size_ == 0)
                    return make_type_id_list();
                storage_[0] = static_cast<type_id_t>(size());
                // Transfer ownership of buffer into the global cache. If an equivalent list
                // already exists, get_or_set_type_id_buf releases `ptr` and returns the old
                // buffer.
                auto ptr = storage_;
                storage_ = nullptr;
                return type_id_list {get_or_set_type_id_buf(ptr)};
            }

            type_id_list type_id_list_builder::copy_to_list() {
                if (size_ == 0)
                    return make_type_id_list();
                storage_[0] = static_cast<type_id_t>(size());
                auto vptr = malloc(size_ * sizeof(type_id_t));
                if (vptr == nullptr)
                    throw std::bad_alloc();
                auto copy = reinterpret_cast<type_id_t *>(vptr);
                memcpy(copy, storage_, size_ * sizeof(type_id_t));
                return type_id_list {get_or_set_type_id_buf(copy)};
            }

        }    // namespace detail
    }        // namespace actor
}    // namespace nil
