//---------------------------------------------------------------------------//
// Copyright (c) 2018-2021 Mikhail Komarov <nemo@nil.foundation>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the Server Side Public License, version 1,
// as published by the author.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// Server Side Public License for more details.
//
// You should have received a copy of the Server Side Public License
// along with this program. If not, see
// <https://github.com/NilFoundation/dbms/blob/master/LICENSE_1_0.txt>.
//---------------------------------------------------------------------------//

#define __user /* empty */    // for xfs includes, below

#include <boost/config.hpp>
#include <boost/filesystem.hpp>
#include <boost/predef.h>

#include <cinttypes>

#include <sys/syscall.h>

#if BOOST_OS_LINUX

#include <sys/statfs.h>
#include <sys/vfs.h>

#elif BOOST_OS_MACOS || BOOST_OS_IOS

#include <sys/param.h>
#include <sys/mount.h>

#endif

#include <sys/time.h>
#include <sys/resource.h>

#include <nil/actor/detail/conversions.hh>
#include <nil/actor/detail/log.hh>

#include <nil/actor/core/task.hh>
#include <nil/actor/core/reactor.hh>
#include <nil/actor/core/memory.hh>
#include <nil/actor/core/posix.hh>

#include <nil/actor/network/packet.hh>
#include <nil/actor/network/stack.hh>
#include <nil/actor/network/posix-stack.hh>

#if BOOST_OS_LINUX
#include <nil/actor/network/native-stack.hh>
#endif

#include <nil/actor/core/resource.hh>
#include <nil/actor/core/print.hh>
#include <nil/actor/core/detail/scollectd-impl.hh>
#include <nil/actor/core/loop.hh>
#include <nil/actor/core/with_scheduling_group.hh>
#include <nil/actor/core/thread.hh>
#include <nil/actor/core/make_task.hh>
#include <nil/actor/core/systemwide_memory_barrier.hh>
#include <nil/actor/core/report_exception.hh>
#include <nil/actor/core/stall_sampler.hh>
#include <nil/actor/core/thread_cputime_clock.hh>
#include <nil/actor/core/abort_on_ebadf.hh>
#include <nil/actor/core/io_queue.hh>
#include <nil/actor/core/scheduling_specific.hh>
#include <nil/actor/core/network_stack_registry.hh>

#include <nil/actor/core/detail/io_desc.hh>
#include <nil/actor/core/detail/buffer_allocator.hh>
#include <nil/actor/core/detail/file-impl.hh>
#include <nil/actor/core/detail/reactor_backend.hh>
#include <nil/actor/core/detail/reactor_backend_selector.hh>
#include <nil/actor/core/detail/syscall_result.hh>
#include <nil/actor/core/detail/thread_pool.hh>
#include <nil/actor/core/detail/syscall_work_queue.hh>
#include <nil/actor/core/detail/cgroup.hh>
#include <nil/actor/core/detail/uname.hh>

#include <cassert>
#include <unistd.h>
#include <fcntl.h>

#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK O_NONBLOCK
#endif

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC O_CLOEXEC
#endif

/* MSG_NOSIGNAL might not be available (i.e. on MacOSX and Solaris).
 *   In this case it gets defined as 0. Safety of such a definition is still
 *   being discussed.
 */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include <sys/eventfd.h>
#include <sys/epoll.h>

#include <boost/lexical_cast.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/range/numeric.hpp>
#include <boost/range/algorithm/sort.hpp>
#include <boost/range/algorithm/remove_if.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/algorithm/clamp.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/version.hpp>

#include <atomic>
#include <dirent.h>
#include <sys/ioctl.h>

#if BOOST_OS_LINUX
#include <linux/types.h>    // for xfs, below
#include <xfs/linux.h>
#define min min /* prevent xfs.h from defining min() as a macro */
#include <xfs/xfs.h>
#undef min
#endif
#ifdef ACTOR_HAVE_DPDK
#include <nil/actor/core/dpdk_rte.hh>
#include <rte_lcore.h>
#include <rte_launch.h>
#endif
#include <nil/actor/core/prefetch.hh>

#include <exception>
#include <regex>
#include <fstream>
#if BOOST_COMP_GNUC
#include <iostream>
#include <system_error>
#include <cxxabi.h>
#endif

#ifdef ACTOR_SHUFFLE_TASK_QUEUE
#include <random>
#endif

#include <sys/mman.h>
#include <sys/utsname.h>
#if BOOST_OS_LINUX
#include <linux/falloc.h>
#include <linux/magic.h>
#endif
#include <nil/actor/detail/backtrace.hh>
#include <nil/actor/detail/spinlock.hh>
#include <nil/actor/detail/print_safe.hh>

#include <sys/sdt.h>

#ifdef HAVE_OSV
#include <osv/newpoll.hh>
#endif

#if BOOST_ARCH_X86
#include <xmmintrin.h>
#endif

#include <nil/actor/detail/defer.hh>
#include <nil/actor/core/alien.hh>
#include <nil/actor/core/metrics.hh>
#include <nil/actor/core/execution_stage.hh>
#include <nil/actor/core/detail/stall_detector.hh>
#include <nil/actor/detail/memory_diagnostics.hh>

//#if BOOST_COMP_GNUC && BOOST_COMP_GNUC >= BOOST_VERSION_NUMBER(7, 0, 0)
//#include <nil/actor/core/exception_hacks.hh>
//#endif
#include <nil/actor/core/exception_hacks.hh>

#include <yaml-cpp/yaml.h>

#ifdef ACTOR_TASK_HISTOGRAM
#include <typeinfo>
#endif

#include <algorithm>
#include <numeric>
#include <limits>

namespace nil {
    namespace actor {

        struct mountpoint_params {
            std::string mountpoint = "none";
            uint64_t read_bytes_rate = std::numeric_limits<uint64_t>::max();
            uint64_t write_bytes_rate = std::numeric_limits<uint64_t>::max();
            uint64_t read_req_rate = std::numeric_limits<uint64_t>::max();
            uint64_t write_req_rate = std::numeric_limits<uint64_t>::max();
        };

    }    // namespace actor
}    // namespace nil

namespace YAML {
    template<>
    struct convert<nil::actor::mountpoint_params> {
        static bool decode(const Node &node, nil::actor::mountpoint_params &mp) {
            using namespace nil::actor;
            mp.mountpoint = node["mountpoint"].as<std::string>().c_str();
            mp.read_bytes_rate = parse_memory_size(node["read_bandwidth"].as<std::string>());
            mp.read_req_rate = parse_memory_size(node["read_iops"].as<std::string>());
            mp.write_bytes_rate = parse_memory_size(node["write_bandwidth"].as<std::string>());
            mp.write_req_rate = parse_memory_size(node["write_iops"].as<std::string>());
            return true;
        }
    };
}    // namespace YAML

namespace nil {
    namespace actor {

        nil::actor::logger actor_logger("actor");
        nil::actor::logger sched_logger("scheduler");

        shard_id reactor::cpu_id() const {
            assert(_id == this_shard_id());
            return _id;
        }

        io_priority_class reactor::register_one_priority_class(sstring name, uint32_t shares) {
            return io_queue::register_one_priority_class(std::move(name), shares);
        }

        future<> reactor::update_shares_for_class(io_priority_class pc, uint32_t shares) {
            return parallel_for_each(
                _io_queues, [pc, shares](auto &queue) { return queue.second->update_shares_for_class(pc, shares); });
        }

        future<> reactor::rename_priority_class(io_priority_class pc, sstring new_name) noexcept {

            return futurize_invoke([pc, new_name = std::move(new_name)]() mutable {
                // Taking the lock here will prevent from newly registered classes
                // to register under the old name (and will prevent undefined
                // behavior since this array is shared cross shards. However, it
                // doesn't prevent the case where a newly registered class (that
                // got registered right after the lock release) will be unnecessarily
                // renamed. This is not a real problem and it is a lot better than
                // holding the lock until all cross shard activity is over.

                try {
                    if (!io_queue::rename_one_priority_class(pc, new_name)) {
                        return make_ready_future<>();
                    }
                } catch (...) {
                    sched_logger.error("exception while trying to rename priority group with id {} to \"{}\" ({})",
                                       pc.id(), new_name, std::current_exception());
                    std::rethrow_exception(std::current_exception());
                }
                return smp::invoke_on_all([pc, new_name = std::move(new_name)] {
                    for (auto &&queue : engine()._io_queues) {
                        queue.second->rename_priority_class(pc, new_name);
                    }
                });
            });
        }

        future<std::tuple<pollable_fd, socket_address>> reactor::do_accept(pollable_fd_state &listenfd) {
            return readable_or_writeable(listenfd).then([this, &listenfd]() mutable {
                socket_address sa;
                listenfd.maybe_no_more_recv();
                auto maybe_fd = listenfd.fd.try_accept(sa, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (!maybe_fd) {
                    // We speculated that we will have an another connection, but got a false
                    // positive. Try again without speculation.
                    return do_accept(listenfd);
                }
                // Speculate that there is another connection on this listening socket, to avoid
                // a task-quota delay. Usually this will fail, but accept is a rare-enough operation
                // that it is worth the false positive in order to withstand a connection storm
                // without having to accept at a rate of 1 per task quota.
                listenfd.speculate_epoll(EPOLLIN);
                pollable_fd pfd(std::move(*maybe_fd), pollable_fd::speculation(EPOLLOUT));
                return make_ready_future<std::tuple<pollable_fd, socket_address>>(
                    std::make_tuple(std::move(pfd), std::move(sa)));
            });
        }

        future<> reactor::do_connect(pollable_fd_state &pfd, socket_address &sa) {
            pfd.fd.connect(sa.u.sa, sa.length());
            return pfd.writeable().then([&pfd]() mutable {
                auto err = pfd.fd.getsockopt<int>(SOL_SOCKET, SO_ERROR);
                if (err != 0) {
                    throw std::system_error(err, std::system_category());
                }
                return make_ready_future<>();
            });
        }

        future<size_t> reactor::do_read_some(pollable_fd_state &fd, void *buffer, size_t len) {
            return readable(fd).then([this, &fd, buffer, len]() mutable {
                auto r = fd.fd.read(buffer, len);
                if (!r) {
                    return do_read_some(fd, buffer, len);
                }
                if (size_t(*r) == len) {
                    fd.speculate_epoll(EPOLLIN);
                }
                return make_ready_future<size_t>(*r);
            });
        }

        future<temporary_buffer<char>> reactor::do_read_some(pollable_fd_state &fd, detail::buffer_allocator *ba) {
            return fd.readable().then([this, &fd, ba] {
                auto buffer = ba->allocate_buffer();
                auto r = fd.fd.read(buffer.get_write(), buffer.size());
                if (!r) {
                    // Speculation failure, try again with real polling this time
                    // Note we release the buffer and will reallocate it when poll
                    // completes.
                    return do_read_some(fd, ba);
                }
                if (size_t(*r) == buffer.size()) {
                    fd.speculate_epoll(EPOLLIN);
                }
                buffer.trim(*r);
                return make_ready_future<temporary_buffer<char>>(std::move(buffer));
            });
        }

        future<size_t> reactor::do_read_some(pollable_fd_state &fd, const std::vector<iovec> &iov) {
            return readable(fd).then([this, &fd, iov = iov]() mutable {
                ::msghdr mh = {};
                mh.msg_iov = &iov[0];
                mh.msg_iovlen = iov.size();
                auto r = fd.fd.recvmsg(&mh, 0);
                if (!r) {
                    return do_read_some(fd, iov);
                }
                if (size_t(*r) == iovec_len(iov)) {
                    fd.speculate_epoll(EPOLLIN);
                }
                return make_ready_future<size_t>(*r);
            });
        }

        future<size_t> reactor::do_write_some(pollable_fd_state &fd, const void *buffer, size_t len) {
            return writeable(fd).then([this, &fd, buffer, len]() mutable {
                auto r = fd.fd.send(buffer, len, MSG_NOSIGNAL);
                if (!r) {
                    return do_write_some(fd, buffer, len);
                }
                if (size_t(*r) == len) {
                    fd.speculate_epoll(EPOLLOUT);
                }
                return make_ready_future<size_t>(*r);
            });
        }

        future<size_t> reactor::do_write_some(pollable_fd_state &fd, net::packet &p) {
            return writeable(fd).then([this, &fd, &p]() mutable {
                BOOST_STATIC_ASSERT_MSG(offsetof(iovec, iov_base) == offsetof(net::fragment, base) &&
                                            sizeof(iovec::iov_base) == sizeof(net::fragment::base) &&
                                            offsetof(iovec, iov_len) == offsetof(net::fragment, size) &&
                                            sizeof(iovec::iov_len) == sizeof(net::fragment::size) &&
                                            alignof(iovec) == alignof(net::fragment) &&
                                            sizeof(iovec) == sizeof(net::fragment),
                                        "net::fragment and iovec should be equivalent");

                iovec *iov = reinterpret_cast<iovec *>(p.fragment_array());
                msghdr mh = {};
                mh.msg_iov = iov;
                mh.msg_iovlen = std::min<size_t>(p.nr_frags(), IOV_MAX);
                auto r = fd.fd.sendmsg(&mh, MSG_NOSIGNAL);
                if (!r) {
                    return do_write_some(fd, p);
                }
                if (size_t(*r) == p.len()) {
                    fd.speculate_epoll(EPOLLOUT);
                }
                return make_ready_future<size_t>(*r);
            });
        }

        future<> reactor::write_all_part(pollable_fd_state &fd, const void *buffer, size_t len, size_t completed) {
            if (completed == len) {
                return make_ready_future<>();
            } else {
                return _backend->write_some(fd, static_cast<const char *>(buffer) + completed, len - completed)
                    .then([&fd, buffer, len, completed, this](size_t part) mutable {
                        return write_all_part(fd, buffer, len, completed + part);
                    });
            }
        }

        future<> reactor::write_all(pollable_fd_state &fd, const void *buffer, size_t len) {
            assert(len);
            return write_all_part(fd, buffer, len, 0);
        }

        future<size_t> pollable_fd_state::read_some(char *buffer, size_t size) {
            return engine()._backend->read_some(*this, buffer, size);
        }

        future<size_t> pollable_fd_state::read_some(uint8_t *buffer, size_t size) {
            return engine()._backend->read_some(*this, buffer, size);
        }

        future<size_t> pollable_fd_state::read_some(const std::vector<iovec> &iov) {
            return engine()._backend->read_some(*this, iov);
        }

        future<temporary_buffer<char>> pollable_fd_state::read_some(detail::buffer_allocator *ba) {
            return engine()._backend->read_some(*this, ba);
        }

        future<size_t> pollable_fd_state::write_some(net::packet &p) {
            return engine()._backend->write_some(*this, p);
        }

        future<> pollable_fd_state::write_all(const char *buffer, size_t size) {
            return engine().write_all(*this, buffer, size);
        }

        future<> pollable_fd_state::write_all(const uint8_t *buffer, size_t size) {
            return engine().write_all(*this, buffer, size);
        }

        future<> pollable_fd_state::write_all(net::packet &p) {
            return write_some(p).then([this, &p](size_t size) {
                if (p.len() == size) {
                    return make_ready_future<>();
                }
                p.trim_front(size);
                return write_all(p);
            });
        }

        future<> pollable_fd_state::readable() {
            return engine().readable(*this);
        }

        future<> pollable_fd_state::writeable() {
            return engine().writeable(*this);
        }

        future<> pollable_fd_state::readable_or_writeable() {
            return engine().readable_or_writeable(*this);
        }

        void pollable_fd_state::abort_reader() {
            engine().abort_reader(*this);
        }

        void pollable_fd_state::abort_writer() {
            engine().abort_writer(*this);
        }

        future<std::tuple<pollable_fd, socket_address>> pollable_fd_state::accept() {
            return engine()._backend->accept(*this);
        }

        future<> pollable_fd_state::connect(socket_address &sa) {
            return engine()._backend->connect(*this, sa);
        }

        future<size_t> pollable_fd_state::recvmsg(struct msghdr *msg) {
            maybe_no_more_recv();
            return engine().readable(*this).then([this, msg] {
                auto r = fd.recvmsg(msg, 0);
                if (!r) {
                    return recvmsg(msg);
                }
                // We always speculate here to optimize for throughput in a workload
                // with multiple outstanding requests. This way the caller can consume
                // all messages without resorting to epoll. However this adds extra
                // recvmsg() call when we hit the empty queue condition, so it may
                // hurt request-response workload in which the queue is empty when we
                // initially enter recvmsg(). If that turns out to be a problem, we can
                // improve speculation by using recvmmsg().
                speculate_epoll(EPOLLIN);

                return make_ready_future<size_t>(*r);
            });
        };

        future<size_t> pollable_fd_state::sendmsg(struct msghdr *msg) {
            maybe_no_more_send();
            return engine().writeable(*this).then([this, msg]() mutable {
                auto r = fd.sendmsg(msg, 0);
                if (!r) {
                    return sendmsg(msg);
                }
                // For UDP this will always speculate. We can't know if there's room
                // or not, but most of the time there should be so the cost of mis-
                // speculation is amortized.
                if (size_t(*r) == iovec_len(msg->msg_iov, msg->msg_iovlen)) {
                    speculate_epoll(EPOLLOUT);
                }
                return make_ready_future<size_t>(*r);
            });
        }

        future<size_t> pollable_fd_state::sendto(socket_address addr, const void *buf, size_t len) {
            maybe_no_more_send();
            return engine().writeable(*this).then([this, buf, len, addr]() mutable {
                auto r = fd.sendto(addr, buf, len, 0);
                if (!r) {
                    return sendto(std::move(addr), buf, len);
                }
                // See the comment about speculation in sendmsg().
                if (size_t(*r) == len) {
                    speculate_epoll(EPOLLOUT);
                }
                return make_ready_future<size_t>(*r);
            });
        }

        namespace detail {

#ifdef ACTOR_TASK_HISTOGRAM

            class task_histogram {
                static constexpr unsigned max_countdown = 1'000'000;
                std::unordered_map<std::type_index, uint64_t> _histogram;
                unsigned _countdown_to_print = max_countdown;

            public:
                void add(const task &t) {
                    ++_histogram[std::type_index(typeid(t))];
                    if (!--_countdown_to_print) {
                        print();
                        _countdown_to_print = max_countdown;
                        _histogram.clear();
                    }
                }
                void print() const {
                    nil::actor::fmt::print("task histogram, {:d} task types {:d} tasks\n", _histogram.size(),
                                           max_countdown - _countdown_to_print);
                    for (auto &&type_count : _histogram) {
                        auto &&type = type_count.first;
                        auto &&count = type_count.second;
                        nil::actor::fmt::print("  {:10d} {}\n", count, type.name());
                    }
                }
            };

            thread_local task_histogram this_thread_task_histogram;

#endif

            void task_histogram_add_task(const task &t) {
#ifdef ACTOR_TASK_HISTOGRAM
                this_thread_task_histogram.add(t);
#endif
            }

        }    // namespace detail

        using namespace std::chrono_literals;
        namespace fs = boost::filesystem;

        using namespace net;

        using namespace detail;
        using namespace detail::linux_abi;

        std::atomic<lowres_clock_impl::steady_rep> lowres_clock_impl::counters::_steady_now;
        std::atomic<lowres_clock_impl::system_rep> lowres_clock_impl::counters::_system_now;
        std::atomic<manual_clock::rep> manual_clock::_now;
        constexpr std::chrono::milliseconds lowres_clock_impl::_granularity;

        constexpr unsigned reactor::max_queues;
        constexpr unsigned reactor::max_aio_per_queue;

        // Broken (returns spurious EIO). Cause/fix unknown.
        bool aio_nowait_supported = false;

        static bool sched_debug() {
            return false;
        }

        template<typename... Args>
        void sched_print(const char *fmt, Args &&...args) {
            if (sched_debug()) {
                sched_logger.trace(fmt, std::forward<Args>(args)...);
            }
        }

        static std::atomic<bool> abort_on_ebadf = {false};

        void set_abort_on_ebadf(bool do_abort) {
            abort_on_ebadf.store(do_abort);
        }

        bool is_abort_on_ebadf_enabled() {
            return abort_on_ebadf.load();
        }

        timespec to_timespec(steady_clock_type::time_point t) {
            using ns = std::chrono::nanoseconds;
            auto n = std::chrono::duration_cast<ns>(t.time_since_epoch()).count();
            return {n / 1'000'000'000, n % 1'000'000'000};
        }

        lowres_clock_impl::lowres_clock_impl() {
            update();
            _timer.set_callback(&lowres_clock_impl::update);
            _timer.arm_periodic(_granularity);
        }

        void lowres_clock_impl::update() noexcept {
            auto const steady_count =
                std::chrono::duration_cast<steady_duration>(base_steady_clock::now().time_since_epoch()).count();

            auto const system_count =
                std::chrono::duration_cast<system_duration>(base_system_clock::now().time_since_epoch()).count();

            counters::_steady_now.store(steady_count, std::memory_order_relaxed);
            counters::_system_now.store(system_count, std::memory_order_relaxed);
        }

        template<typename Clock>
        inline timer<Clock>::~timer() {
            if (_queued) {
                engine().del_timer(this);
            }
        }

        template<typename Clock>
        inline void timer<Clock>::arm(time_point until, boost::optional<duration> period) noexcept {
            arm_state(until, period);
            engine().add_timer(this);
        }

        template<typename Clock>
        inline void timer<Clock>::readd_periodic() noexcept {
            arm_state(Clock::now() + _period.value(), {_period.value()});
            engine().queue_timer(this);
        }

        template<typename Clock>
        inline bool timer<Clock>::cancel() noexcept {
            if (!_armed) {
                return false;
            }
            _armed = false;
            if (_queued) {
                engine().del_timer(this);
                _queued = false;
            }
            return true;
        }

        template class timer<steady_clock_type>;
        template class timer<lowres_clock>;
        template class timer<manual_clock>;

        reactor::signals::signals() : _pending_signals(0) {
        }

        reactor::signals::~signals() {
            sigset_t mask;
            sigfillset(&mask);
            ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
        }

        reactor::signals::signal_handler::signal_handler(int signo, noncopyable_function<void()> &&handler) :
            _handler(std::move(handler)) {
        }

        void reactor::signals::handle_signal(int signo, noncopyable_function<void()> &&handler) {
            _signal_handlers.emplace(std::piecewise_construct, std::make_tuple(signo),
                                     std::make_tuple(signo, std::move(handler)));

            struct sigaction sa;
            sa.sa_sigaction = [](int sig, siginfo_t *info, void *p) {
                engine()._backend->signal_received(sig, info, p);
            };
            sa.sa_mask = make_empty_sigset_mask();
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            auto r = ::sigaction(signo, &sa, nullptr);
            throw_system_error_on(r == -1);
            auto mask = make_sigset_mask(signo);
            r = ::pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
            throw_pthread_error(r);
        }

        void reactor::signals::handle_signal_once(int signo, noncopyable_function<void()> &&handler) {
            return handle_signal(signo, [fired = false, handler = std::move(handler)]() mutable {
                if (!fired) {
                    fired = true;
                    handler();
                }
            });
        }

        bool reactor::signals::poll_signal() {
            auto signals = _pending_signals.load(std::memory_order_relaxed);
            if (signals) {
                _pending_signals.fetch_and(~signals, std::memory_order_relaxed);
                for (size_t i = 0; i < sizeof(signals) * 8; i++) {
                    if (signals & (1ull << i)) {
                        _signal_handlers.at(i)._handler();
                    }
                }
            }
            return signals;
        }

        bool reactor::signals::pure_poll_signal() const {
            return _pending_signals.load(std::memory_order_relaxed);
        }

        void reactor::signals::action(int signo, siginfo_t *siginfo, void *ignore) {
            engine().start_handling_signal();
            engine()._signals._pending_signals.fetch_or(1ull << signo, std::memory_order_relaxed);
        }

        void reactor::signals::failed_to_handle(int signo) {
            char tname[64];
            pthread_getname_np(pthread_self(), tname, sizeof(tname));
#if BOOST_OS_LINUX
            uint64_t tid = syscall(SYS_gettid);
#elif BOOST_OS_MACOS || BOOST_OS_IOS
            uint64_t tid;
            pthread_threadid_np(NULL, &tid);
#endif
            actor_logger.error("Failed to handle signal {} on thread {} ({}): engine not ready", signo, tid, tname);
        }

        void reactor::handle_signal(int signo, noncopyable_function<void()> &&handler) {
            _signals.handle_signal(signo, std::move(handler));
        }

        // Accumulates an in-memory backtrace and flush to stderr eventually.
        // Async-signal safe.
        class backtrace_buffer {
            static constexpr unsigned _max_size = 8 << 10;
            unsigned _pos = 0;
            char _buf[_max_size];

        public:
            void flush() noexcept {
                print_safe(_buf, _pos);
                _pos = 0;
            }

            void append(const char *str, size_t len) noexcept {
                if (_pos + len >= _max_size) {
                    flush();
                }
                memcpy(_buf + _pos, str, len);
                _pos += len;
            }

            void append(const char *str) noexcept {
                append(str, strlen(str));
            }

            template<typename Integral>
            void append_decimal(Integral n) noexcept {
                char buf[sizeof(n) * 3];
                auto len = convert_decimal_safe(buf, sizeof(buf), n);
                append(buf, len);
            }

            template<typename Integral>
            void append_hex(Integral ptr) noexcept {
                char buf[sizeof(ptr) * 2];
                convert_zero_padded_hex_safe(buf, sizeof(buf), ptr);
                append(buf, sizeof(buf));
            }

            void append_backtrace() noexcept {
                backtrace([this](frame f) {
                    append("  ");
                    if (!f.so->name.empty()) {
                        append(f.so->name.c_str(), f.so->name.size());
                        append("+");
                    }

                    append("0x");
                    append_hex(f.addr);
                    append("\n");
                });
            }
        };

        static void print_with_backtrace(backtrace_buffer &buf) noexcept {
            if (local_engine) {
                buf.append(" on shard ");
                buf.append_decimal(this_shard_id());
            }

            buf.append(".\nBacktrace:\n");
            buf.append_backtrace();
            buf.flush();
        }

        static void print_with_backtrace(const char *cause) noexcept {
            backtrace_buffer buf;
            buf.append(cause);
            print_with_backtrace(buf);
        }

        // Installs signal handler stack for current thread.
        // The stack remains installed as long as the returned object is kept alive.
        // When it goes out of scope the previous handler is restored.
        static decltype(auto) install_signal_handler_stack() {
            size_t size = SIGSTKSZ;
            auto mem = std::make_unique<char[]>(size);
            stack_t stack;
            stack_t prev_stack;
            stack.ss_sp = mem.get();
            stack.ss_flags = 0;
            stack.ss_size = size;
            auto r = sigaltstack(&stack, &prev_stack);
            throw_system_error_on(r == -1);
            return defer([mem = std::move(mem), prev_stack]() mutable {
                try {
                    auto r = sigaltstack(&prev_stack, NULL);
                    throw_system_error_on(r == -1);
                } catch (...) {
                    mem.release();    // We failed to restore previous stack, must leak it.
                    actor_logger.error("Failed to restore signal stack: {}", std::current_exception());
                }
            });
        }

        reactor::task_queue::task_queue(unsigned id, sstring name, float shares) :
            _shares(std::max(shares, 1.0f)), _reciprocal_shares_times_2_power_32((uint64_t(1) << 32) / _shares),
            _id(id), _ts(std::chrono::steady_clock::now()), _name(name) {
            register_stats();
        }

        void reactor::task_queue::register_stats() {
            nil::actor::metrics::metric_groups new_metrics;
            namespace sm = nil::actor::metrics;
            static auto group = sm::label("group");
            auto group_label = group(_name);
            new_metrics.add_group(
                "scheduler",
                {
                    sm::make_counter(
                        "runtime_ms",
                        [this] { return std::chrono::duration_cast<std::chrono::milliseconds>(_runtime).count(); },
                        sm::description(
                            "Accumulated runtime of this task queue; an increment rate of 1000ms per second "
                            "indicates full utilization"),
                        {group_label}),
                    sm::make_counter(
                        "waittime_ms",
                        [this] { return std::chrono::duration_cast<std::chrono::milliseconds>(_waittime).count(); },
                        sm::description(
                            "Accumulated waittime of this task queue; an increment rate of 1000ms per second "
                            "indicates queue is waiting for something (e.g. IO)"),
                        {group_label}),
                    sm::make_counter(
                        "starvetime_ms",
                        [this] { return std::chrono::duration_cast<std::chrono::milliseconds>(_starvetime).count(); },
                        sm::description(
                            "Accumulated starvation time of this task queue; an increment rate of 1000ms per "
                            "second indicates the scheduler feels really bad"),
                        {group_label}),
                    sm::make_counter("tasks_processed", _tasks_processed,
                                     sm::description("Count of tasks executing on this queue; indicates together with "
                                                     "runtime_ms indicates length of tasks"),
                                     {group_label}),
                    sm::make_gauge(
                        "queue_length", [this] { return _q.size(); },
                        sm::description("Size of backlog on this queue, in tasks; indicates whether the queue "
                                        "is busy and/or contended"),
                        {group_label}),
                    sm::make_gauge("shares", [this] { return _shares; },
                                   sm::description("Shares allocated to this queue"), {group_label}),
                    sm::make_derive(
                        "time_spent_on_task_quota_violations_ms",
                        [this] { return _time_spent_on_task_quota_violations / 1ms; },
                        sm::description("Total amount in milliseconds we were in violation of the task quota"),
                        {group_label}),
                });
            _metrics = std::exchange(new_metrics, {});
        }

        void reactor::task_queue::rename(sstring new_name) {
            if (_name != new_name) {
                _name = new_name;
                register_stats();
            }
        }

#if BOOST_COMP_CLANG
        __attribute__((no_sanitize("undefined")))    // multiplication below may overflow; we check for that
#elif BOOST_COMP_GNUC
        [[gnu::no_sanitize_undefined]]
#endif
        inline int64_t
            reactor::task_queue::to_vruntime(sched_clock::duration runtime) const {
            auto scaled = (runtime.count() * _reciprocal_shares_times_2_power_32) >> 32;
            // Prevent overflow from returning ridiculous values
            return std::max<int64_t>(scaled, 0);
        }

        void reactor::task_queue::set_shares(float shares) noexcept {
            _shares = std::max(shares, 1.0f);
            _reciprocal_shares_times_2_power_32 = (uint64_t(1) << 32) / _shares;
        }

        void reactor::account_runtime(task_queue &tq, sched_clock::duration runtime) {
            if (runtime > (2 * _task_quota)) {
                tq._time_spent_on_task_quota_violations += runtime - _task_quota;
            }
            tq._vruntime += tq.to_vruntime(runtime);
            tq._runtime += runtime;
        }

        void reactor::account_idle(sched_clock::duration runtime) {
            // anything to do here?
        }

        struct reactor::task_queue::indirect_compare {
            bool operator()(const task_queue *tq1, const task_queue *tq2) const {
                return tq1->_vruntime < tq2->_vruntime;
            }
        };

        reactor::reactor(unsigned id, reactor_backend_selector rbs, reactor_config cfg) :
            _cfg(cfg), _notify_eventfd(file_desc::eventfd(0, EFD_CLOEXEC)),
            _task_quota_timer(file_desc::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)), _id(id),
#ifdef HAVE_OSV
            _timer_thread([&] { timer_thread_func(); },
                          sched::thread::attr().stack(4096).name("timer_thread").pin(sched::cpu::current())),
            _engine_thread(sched::thread::current()),
#endif
            _cpu_started(0), _cpu_stall_detector(std::make_unique<cpu_stall_detector>()),
            _reuseport(posix_reuseport_detect()),
            _thread_pool(std::make_unique<thread_pool>(this, nil::actor::format("syscall-{}", id))) {
            /*
             * The _backend assignment is here, not on the initialization list as
             * the chosen backend constructor may want to handle signals and thus
             * needs the _signals._signal_handlers map to be initialized.
             */
            _backend = rbs.create(*this);
            *detail::get_scheduling_group_specific_thread_local_data_ptr() = &_scheduling_group_specific_data;
            _task_queues.push_back(std::make_unique<task_queue>(0, "main", 1000));
            _task_queues.push_back(std::make_unique<task_queue>(1, "atexit", 1000));
            _at_destroy_tasks = _task_queues.back().get();
            g_need_preempt = &_preemption_monitor;
            nil::actor::thread_impl::init();
            _backend->start_tick();

#ifdef HAVE_OSV
            _timer_thread.start();
#else
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, cpu_stall_detector::signal_number());
            auto r = ::pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
            assert(r == 0);
#endif
            memory::set_reclaim_hook([this](std::function<void()> reclaim_fn) {
                add_high_priority_task(make_task(default_scheduling_group(), [fn = std::move(reclaim_fn)] { fn(); }));
            });
        }

        reactor::~reactor() {
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, cpu_stall_detector::signal_number());
            auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
            assert(r == 0);

            _backend->stop_tick();
            auto eraser = [](auto &list) {
                while (!list.empty()) {
                    auto &timer = *list.begin();
                    timer.cancel();
                }
            };
            eraser(_expired_timers);
            eraser(_expired_lowres_timers);
            eraser(_expired_manual_timers);
            auto &sg_data = _scheduling_group_specific_data;
            for (auto &&tq : _task_queues) {
                if (tq) {
                    auto &this_sg = sg_data.per_scheduling_group_data[tq->_id];
                    // The following line will preserve the convention that constructor and destructor functions
                    // for the per sg values are called in the context of the containing scheduling group.
                    *detail::current_scheduling_group_ptr() = scheduling_group(tq->_id);
                    for (size_t key : boost::irange<size_t>(0, sg_data.scheduling_group_key_configs.size())) {
                        void *val = this_sg.specific_vals[key];
                        if (val) {
                            if (sg_data.scheduling_group_key_configs[key].destructor) {
                                sg_data.scheduling_group_key_configs[key].destructor(val);
                            }
                            free(val);
                            this_sg.specific_vals[key] = nullptr;
                        }
                    }
                }
            }
        }

        future<> reactor::readable(pollable_fd_state &fd) {
            return _backend->readable(fd);
        }

        future<> reactor::writeable(pollable_fd_state &fd) {
            return _backend->writeable(fd);
        }

        future<> reactor::readable_or_writeable(pollable_fd_state &fd) {
            return _backend->readable_or_writeable(fd);
        }

        void reactor::abort_reader(pollable_fd_state &fd) {
            // TCP will respond to shutdown(SHUT_RD) by returning ECONNABORT on the next read,
            // but UDP responds by returning AGAIN. The no_more_recv flag tells us to convert
            // EAGAIN to ECONNABORT in that case.
            fd.no_more_recv = true;
            return fd.fd.shutdown(SHUT_RD);
        }

        void reactor::abort_writer(pollable_fd_state &fd) {
            // TCP will respond to shutdown(SHUT_WR) by returning ECONNABORT on the next write,
            // but UDP responds by returning AGAIN. The no_more_recv flag tells us to convert
            // EAGAIN to ECONNABORT in that case.
            fd.no_more_send = true;
            return fd.fd.shutdown(SHUT_WR);
        }

        void reactor::set_strict_dma(bool value) {
            _strict_o_direct = value;
        }

        void reactor::set_bypass_fsync(bool value) {
            _bypass_fsync = value;
        }

        void reactor::reset_preemption_monitor() {
            return _backend->reset_preemption_monitor();
        }

        void reactor::request_preemption() {
            return _backend->request_preemption();
        }

        void reactor::start_handling_signal() {
            return _backend->start_handling_signal();
        }

        cpu_stall_detector::cpu_stall_detector(const cpu_stall_detector_config &cfg) : _shard_id(this_shard_id()) {
            // glib's backtrace() calls dlopen("libgcc_s.so.1") once to resolve unwind related symbols.
            // If first stall detector invocation happens during another dlopen() call the calling thread
            // will deadlock. The dummy call here makes sure that backtrace's initialization happens in
            // a safe place.
            backtrace([](frame) {});
            update_config(cfg);
#if BOOST_OS_LINUX
            struct sigevent sev = {};
            sev.sigev_notify = SIGEV_THREAD_ID;
            sev.sigev_signo = signal_number();
            sev._sigev_un._tid = syscall(SYS_gettid);
            int err = timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &_timer);
            if (err) {
                throw std::system_error(std::error_code(err, std::system_category()));
            }
#elif BOOST_OS_MACOS || BOOST_OS_IOS
            _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
#if BOOST_COMP_CLANG
            dispatch_source_set_event_handler(_timer, ^{
                raise(signal_number());
            });
#else
            dispatch_source_set_event_handler_f(_timer, &raise(signal_number()));
#endif
#endif

            namespace sm = nil::actor::metrics;

            _metrics.add_group(
                "stall_detector",
                {sm::make_derive(
                    "reported", _total_reported,
                    sm::description("Total number of reported stalls, look in the traces for the exact reason"))});

            // note: if something is added here that can, it should take care to destroy _timer.
        }

        cpu_stall_detector::~cpu_stall_detector() {
#if BOOST_OS_LINUX
            timer_delete(_timer);
#elif BOOST_OS_MACOS || BOOST_OS_IOS
            dispatch_source_cancel(_timer);
#endif
        }

        cpu_stall_detector_config cpu_stall_detector::get_config() const {
            return _config;
        }

        void cpu_stall_detector::update_config(const cpu_stall_detector_config &cfg) {
            _config = cfg;
            _threshold = std::chrono::duration_cast<std::chrono::steady_clock::duration>(cfg.threshold);
            _slack = std::chrono::duration_cast<std::chrono::steady_clock::duration>(cfg.threshold * cfg.slack);
            _stall_detector_reports_per_minute = cfg.stall_detector_reports_per_minute;
            _max_reports_per_minute = cfg.stall_detector_reports_per_minute;
            _rearm_timer_at = std::chrono::steady_clock::now();
        }

        void cpu_stall_detector::maybe_report() {
            if (_reported++ < _max_reports_per_minute) {
                generate_trace();
            }
        }
        // We use a tick at every timer firing so we can report suppressed backtraces.
        // Best case it's a correctly predicted branch. If a backtrace had happened in
        // the near past it's an increment and two branches.
        //
        // We can do it a cheaper if we don't report suppressed backtraces.
        void cpu_stall_detector::on_signal() {
            auto tasks_processed = engine().tasks_processed();
            auto last_seen = _last_tasks_processed_seen.load(std::memory_order_relaxed);
            if (!last_seen) {
                return;                                   // stall detector in not active
            } else if (last_seen == tasks_processed) {    // no task was processed - report
                maybe_report();
                _report_at <<= 1;
            } else {
                _last_tasks_processed_seen.store(tasks_processed, std::memory_order_relaxed);
            }
            arm_timer();
        }

        void cpu_stall_detector::report_suppressions(std::chrono::steady_clock::time_point now) {
            if (now > _minute_mark + 60s) {
                if (_reported > _max_reports_per_minute) {
                    auto suppressed = _reported - _max_reports_per_minute;
                    backtrace_buffer buf;
                    // Reuse backtrace buffer infrastructure so we don't have to allocate here
                    buf.append("Rate-limit: suppressed ");
                    buf.append_decimal(suppressed);
                    suppressed == 1 ? buf.append(" backtrace") : buf.append(" backtraces");
                    buf.append(" on shard ");
                    buf.append_decimal(_shard_id);
                    buf.append("\n");
                    buf.flush();
                }
                _reported = 0;
                _minute_mark = now;
            }
        }

        void cpu_stall_detector::arm_timer() {
#if BOOST_OS_LINUX
            auto its = posix::to_relative_itimerspec(_threshold * _report_at + _slack, 0s);
            timer_settime(_timer, 0, &its, nullptr);
#elif BOOST_OS_MACOS || BOOST_OS_IOS
            dispatch_source_set_timer(
                _timer,
                dispatch_walltime(
                    NULL,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(_threshold * _report_at + _slack).count()),
                1ull * NSEC_PER_SEC, 0);
#endif
        }

        void cpu_stall_detector::start_task_run(std::chrono::steady_clock::time_point now) {
            if (now > _rearm_timer_at) {
                report_suppressions(now);
                _report_at = 1;
                _run_started_at = now;
                _rearm_timer_at = now + _threshold * _report_at;
                arm_timer();
            }
            _last_tasks_processed_seen.store(engine().tasks_processed(), std::memory_order_relaxed);
            std::atomic_signal_fence(
                std::memory_order_release);    // Don't delay this write, so the signal handler can see it
        }

        void cpu_stall_detector::end_task_run(std::chrono::steady_clock::time_point now) {
            std::atomic_signal_fence(
                std::memory_order_acquire);    // Don't hoist this write, so the signal handler can see it
            _last_tasks_processed_seen.store(0, std::memory_order_relaxed);
        }

        void cpu_stall_detector::start_sleep() {
#if BOOST_OS_LINUX
            auto its = posix::to_relative_itimerspec(0s, 0s);
            timer_settime(_timer, 0, &its, nullptr);
#elif BOOST_OS_MACOS || BOOST_OS_IOS
            dispatch_source_set_timer(_timer, dispatch_walltime(NULL, 0), 1ull * NSEC_PER_SEC, 0);
#endif
            _rearm_timer_at = std::chrono::steady_clock::now();
        }

        void cpu_stall_detector::end_sleep() {
        }

        void reactor::update_blocked_reactor_notify_ms(std::chrono::milliseconds ms) {
            auto cfg = _cpu_stall_detector->get_config();
            if (ms != cfg.threshold) {
                cfg.threshold = ms;
                _cpu_stall_detector->update_config(cfg);
                actor_logger.info("updated: blocked-reactor-notify-ms={}", ms.count());
            }
        }

        std::chrono::milliseconds reactor::get_blocked_reactor_notify_ms() const {
            auto d = _cpu_stall_detector->get_config().threshold;
            return std::chrono::duration_cast<std::chrono::milliseconds>(d);
        }

        void reactor::set_stall_detector_report_function(std::function<void()> report) {
            auto cfg = _cpu_stall_detector->get_config();
            cfg.report = std::move(report);
            _cpu_stall_detector->update_config(std::move(cfg));
        }

        std::function<void()> reactor::get_stall_detector_report_function() const {
            return _cpu_stall_detector->get_config().report;
        }

        void reactor::block_notifier(int) {
            engine()._cpu_stall_detector->on_signal();
        }

        void cpu_stall_detector::generate_trace() {
            auto delta = std::chrono::steady_clock::now() - _run_started_at;

            _total_reported++;
            if (_config.report) {
                _config.report();
                return;
            }

            backtrace_buffer buf;
            buf.append("Reactor stalled for ");
            buf.append_decimal(uint64_t(delta / 1ms));
            buf.append(" ms");
            print_with_backtrace(buf);
        }

        template<typename T, typename E, typename EnableFunc>
        void reactor::complete_timers(T &timers, E &expired_timers,
                                      EnableFunc &&enable_fn) noexcept(noexcept(enable_fn())) {
            expired_timers = timers.expire(timers.now());
            for (auto &t : expired_timers) {
                t._expired = true;
            }
            const auto prev_sg = current_scheduling_group();
            while (!expired_timers.empty()) {
                auto t = &*expired_timers.begin();
                expired_timers.pop_front();
                t->_queued = false;
                if (t->_armed) {
                    t->_armed = false;
                    if (t->_period) {
                        t->readd_periodic();
                    }
                    try {
                        *detail::current_scheduling_group_ptr() = t->_sg;
                        t->_callback();
                    } catch (...) {
                        actor_logger.error("Timer callback failed: {}", std::current_exception());
                    }
                }
            }
            // complete_timers() can be called from the context of run_tasks()
            // as well so we need to restore the previous scheduling group (set by run_tasks()).
            *detail::current_scheduling_group_ptr() = prev_sg;
            enable_fn();
        }

#ifdef HAVE_OSV
        void reactor::timer_thread_func() {
            sched::timer tmr(*sched::thread::current());
            WITH_LOCK(_timer_mutex) {
                while (!_stopped) {
                    if (_timer_due != 0) {
                        set_timer(tmr, _timer_due);
                        _timer_cond.wait(_timer_mutex, &tmr);
                        if (tmr.expired()) {
                            _timer_due = 0;
                            _engine_thread->unsafe_stop();
                            _pending_tasks.push_front(make_task(default_scheduling_group(), [this] {
                                complete_timers(_timers, _expired_timers, [this] {
                                    if (!_timers.empty()) {
                                        enable_timer(_timers.get_next_timeout());
                                    }
                                });
                            }));
                            _engine_thread->wake();
                        } else {
                            tmr.cancel();
                        }
                    } else {
                        _timer_cond.wait(_timer_mutex);
                    }
                }
            }
        }

        void reactor::set_timer(sched::timer &tmr, s64 t) {
            using namespace osv::clock;
            tmr.set(wall::time_point(std::chrono::nanoseconds(t)));
        }
#endif

        void reactor::configure(const boost::program_options::variables_map &vm) {
            _network_stack_ready =
                vm.count("network-stack") ?
                    network_stack_registry::create(sstring(vm["network-stack"].as<std::string>()), vm) :
                    network_stack_registry::create(vm);

            _handle_sigint = !vm.count("no-handle-interrupt");
            auto task_quota = vm["task-quota-ms"].as<double>() * 1ms;
            _task_quota = std::chrono::duration_cast<sched_clock::duration>(task_quota);

            auto blocked_time = vm["blocked-reactor-notify-ms"].as<unsigned>() * 1ms;
            cpu_stall_detector_config csdc;
            csdc.threshold = blocked_time;
            csdc.stall_detector_reports_per_minute = vm["blocked-reactor-reports-per-minute"].as<unsigned>();
            _cpu_stall_detector->update_config(csdc);

            _max_task_backlog = vm["max-task-backlog"].as<unsigned>();
            _max_poll_time = vm["idle-poll-time-us"].as<unsigned>() * 1us;
            if (vm.count("poll-mode")) {
                _max_poll_time = std::chrono::nanoseconds::max();
            }
            if (vm.count("overprovisioned") && vm["idle-poll-time-us"].defaulted() && !vm.count("poll-mode")) {
                _max_poll_time = 0us;
            }
            set_strict_dma(!vm.count("relaxed-dma"));
            if (!vm["poll-aio"].as<bool>() || (vm["poll-aio"].defaulted() && vm.count("overprovisioned"))) {
                _aio_eventfd = pollable_fd(file_desc::eventfd(0, 0));
            }
            set_bypass_fsync(vm["unsafe-bypass-fsync"].as<bool>());
            _force_io_getevents_syscall = vm["force-aio-syscalls"].as<bool>();
            aio_nowait_supported = vm["linux-aio-nowait"].as<bool>();
            _have_aio_fsync = vm["aio-fsync"].as<bool>();
        }

        pollable_fd reactor::posix_listen(socket_address sa, listen_options opts) {
            auto specific_protocol = (int)(opts.proto);
            if (sa.is_af_unix()) {
                // no type-safe way to create listen_opts with proto=0
                specific_protocol = 0;
            }
            static auto somaxconn = [] {
                boost::optional<int> result;
                std::ifstream ifs("/proc/sys/net/core/somaxconn");
                if (ifs) {
                    result = 0;
                    ifs >> *result;
                }
                return result;
            }();
            if (somaxconn && *somaxconn < opts.listen_backlog) {
                fmt::print(
                    "Warning: /proc/sys/net/core/somaxconn is set to {:d} "
                    "which is lower than the backlog parameter {:d} used for listen(), "
                    "please change it with `sysctl -w net.core.somaxconn={:d}`\n",
                    *somaxconn, opts.listen_backlog, opts.listen_backlog);
            }

            file_desc fd =
                file_desc::socket(sa.u.sa.sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, specific_protocol);
            if (opts.reuse_address) {
                fd.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1);
            }
            if (_reuseport && !sa.is_af_unix())
                fd.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);

            try {
                fd.bind(sa.u.sa, sa.length());
                fd.listen(opts.listen_backlog);
            } catch (const std::system_error &s) {
                throw std::system_error(s.code(), fmt::format("posix_listen failed for address {}", sa));
            }

            return pollable_fd(std::move(fd));
        }

        bool reactor::posix_reuseport_detect() {
            return false;    // FIXME: reuseport currently leads to heavy load imbalance. Until we fix that, just
                             // disable it unconditionally.
            try {
                file_desc fd = file_desc::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
                fd.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
                return true;
            } catch (std::system_error &e) {
                return false;
            }
        }

        void pollable_fd_state::maybe_no_more_recv() {
            if (no_more_recv) {
                throw std::system_error(std::error_code(ECONNABORTED, std::system_category()));
            }
        }

        void pollable_fd_state::maybe_no_more_send() {
            if (no_more_send) {
                throw std::system_error(std::error_code(ECONNABORTED, std::system_category()));
            }
        }

        void pollable_fd_state::forget() {
            engine()._backend->forget(*this);
        }

        void intrusive_ptr_release(pollable_fd_state *fd) {
            if (!--fd->_refs) {
                fd->forget();
            }
        }

        pollable_fd::pollable_fd(file_desc fd, pollable_fd::speculation speculate) :
            _s(engine()._backend->make_pollable_fd_state(std::move(fd), speculate)) {
        }

        void pollable_fd::shutdown(int how) {
            engine()._backend->shutdown(*_s, how);
        }

        pollable_fd reactor::make_pollable_fd(socket_address sa, int proto) {
            file_desc fd = file_desc::socket(sa.u.sa.sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, proto);
            return pollable_fd(std::move(fd));
        }

        future<> reactor::posix_connect(pollable_fd pfd, socket_address sa, socket_address local) {
#ifdef IP_BIND_ADDRESS_NO_PORT
            if (!sa.is_af_unix()) {
                try {
                    // do not reserve an ephemeral port when using bind() with port number 0.
                    // connect() will handle it later. The reason for that is that bind() may fail
                    // to allocate a port while connect will success, this is because bind() does not
                    // know dst address and has to find globally unique local port.
                    pfd.get_file_desc().setsockopt(SOL_IP, IP_BIND_ADDRESS_NO_PORT, 1);
                } catch (std::system_error &err) {
                    if (err.code() != std::error_code(ENOPROTOOPT, std::system_category())) {
                        throw;
                    }
                }
            }
#endif
            if (!local.is_wildcard()) {
                // call bind() only if local address is not wildcard
                pfd.get_file_desc().bind(local.u.sa, local.length());
            }
            return pfd.connect(sa).finally([pfd] {});
        }

        server_socket reactor::listen(socket_address sa, listen_options opt) {
            return server_socket(_network_stack->listen(sa, opt));
        }

        future<connected_socket> reactor::connect(socket_address sa) {
            return _network_stack->connect(sa);
        }

        future<connected_socket> reactor::connect(socket_address sa, socket_address local, transport proto) {
            return _network_stack->connect(sa, local, proto);
        }

        sstring io_request::opname() const {
            switch (_op) {
                case io_request::operation::fdatasync:
                    return "fdatasync";
                case io_request::operation::write:
                    return "write";
                case io_request::operation::writev:
                    return "vectored write";
                case io_request::operation::read:
                    return "read";
                case io_request::operation::readv:
                    return "vectored read";
                case io_request::operation::recv:
                    return "recv";
                case io_request::operation::recvmsg:
                    return "recvmsg";
                case io_request::operation::send:
                    return "send";
                case io_request::operation::sendmsg:
                    return "sendmsg";
                case io_request::operation::accept:
                    return "accept";
                case io_request::operation::connect:
                    return "connect";
                case io_request::operation::poll_add:
                    return "poll add";
                case io_request::operation::poll_remove:
                    return "poll remove";
                case io_request::operation::cancel:
                    return "cancel";
            }
            std::abort();
        }

        void io_completion::complete_with(ssize_t res) {
            if (res >= 0) {
                complete(res);
                return;
            }

            ++engine()._io_stats.aio_errors;
            try {
                throw_kernel_error(res);
            } catch (...) {
                set_exception(std::current_exception());
            }
        }

        bool reactor::flush_pending_aio() {
            for (auto &ioq : _io_queues) {
                ioq.second->poll_io_queue();
            }
            return false;
        }

        steady_clock_type::time_point reactor::next_pending_aio() const noexcept {
            steady_clock_type::time_point next = steady_clock_type::time_point::max();

            for (auto &ioq : _io_queues) {
                steady_clock_type::time_point n = ioq.second->next_pending_aio();
                if (n < next) {
                    next = std::move(n);
                }
            }

            return next;
        }

        bool reactor::reap_kernel_completions() {
            return _backend->reap_kernel_completions();
        }

        const io_priority_class &default_priority_class() {
            static thread_local auto shard_default_class = [] {
                return engine().register_one_priority_class("default", 1);
            }();
            return shard_default_class;
        }

        future<size_t> reactor::submit_io_read(io_queue *ioq, const io_priority_class &pc, size_t len, io_request req,
                                               io_intent *intent) noexcept {
            ++_io_stats.aio_reads;
            _io_stats.aio_read_bytes += len;
            return ioq->queue_request(pc, len, std::move(req), intent);
        }

        future<size_t> reactor::submit_io_write(io_queue *ioq, const io_priority_class &pc, size_t len, io_request req,
                                                io_intent *intent) noexcept {
            ++_io_stats.aio_writes;
            _io_stats.aio_write_bytes += len;
            return ioq->queue_request(pc, len, std::move(req), intent);
        }

        namespace detail {

            size_t sanitize_iovecs(std::vector<iovec> &iov, size_t disk_alignment) noexcept {
                if (iov.size() > IOV_MAX) {
                    iov.resize(IOV_MAX);
                }
                auto length =
                    boost::accumulate(iov | boost::adaptors::transformed(std::mem_fn(&iovec::iov_len)), size_t(0));
                while (auto rest = length & (disk_alignment - 1)) {
                    if (iov.back().iov_len <= rest) {
                        length -= iov.back().iov_len;
                        iov.pop_back();
                    } else {
                        iov.back().iov_len -= rest;
                        length -= rest;
                    }
                }
                return length;
            }

        }    // namespace detail

        future<file> reactor::open_file_dma(std::string_view nameref, open_flags flags,
                                            file_open_options options) noexcept {
            return do_with(
                static_cast<int>(flags), std::move(options),
                [this, nameref](auto &open_flags, file_open_options &options) {
                    sstring name(nameref);
                    return _thread_pool
                        ->submit<syscall_result<int>>([name, &open_flags, &options, strict_o_direct = _strict_o_direct,
                                                       bypass_fsync = _bypass_fsync]() mutable {
                            // We want O_DIRECT, except in two cases:
                            //   - tmpfs (which doesn't support it, but works fine anyway)
                            //   - strict_o_direct == false (where we forgive it being not supported)
                            // Because open() with O_DIRECT will fail, we open it without O_DIRECT, try
                            // to update it to O_DIRECT with fcntl(), and if that fails, see if we
                            // can forgive it.
                            auto is_tmpfs = [](int fd) {
                                struct ::statfs buf;
                                auto r = ::fstatfs(fd, &buf);
                                if (r == -1) {
                                    return false;
                                }
                                return buf.f_type == 0x01021994;    // TMPFS_MAGIC
                            };
                            open_flags |= O_CLOEXEC;
                            if (bypass_fsync) {
                                open_flags &= ~O_DSYNC;
                            }
                            auto mode = static_cast<mode_t>(options.create_permissions);
                            int fd = ::open(name.c_str(), open_flags, mode);
                            if (fd == -1) {
                                return wrap_syscall<int>(fd);
                            }
#if BOOST_OS_LINUX
                            int r = ::fcntl(fd, F_SETFL, open_flags | O_DIRECT);
#elif BOOST_OS_MACOS || BOOST_OS_IOS
                            int r = ::fcntl(fd, F_SETFL, open_flags | F_NOCACHE);
#endif
                            auto maybe_ret = wrap_syscall<int>(r);    // capture errno (should be EINVAL)
                            if (r == -1 && strict_o_direct && !is_tmpfs(fd)) {
                                ::close(fd);
                                return maybe_ret;
                            }
#if BOOST_OS_LINUX
                            if (fd != -1) {
                                fsxattr attr = {};
                                if (options.extent_allocation_size_hint) {
                                    attr.fsx_xflags |= XFS_XFLAG_EXTSIZE;
                                    attr.fsx_extsize = options.extent_allocation_size_hint;
                                }
                                // Ignore error; may be !xfs, and just a hint anyway
                                ::ioctl(fd, XFS_IOC_FSSETXATTR, &attr);
                            }
#endif
                            return wrap_syscall<int>(fd);
                        })
                        .then([&options, name = std::move(name), &open_flags](syscall_result<int> sr) {
                            sr.throw_fs_exception_if_error("open failed", name);
                            return make_file_impl(sr.result, options, open_flags);
                        })
                        .then([](shared_ptr<file_impl> impl) { return make_ready_future<file>(std::move(impl)); });
                });
        }

        future<> reactor::remove_file(std::string_view pathname) noexcept {
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([pathname] {
                return engine()
                    ._thread_pool
                    ->submit<syscall_result<int>>(
                        [pathname = sstring(pathname)] { return wrap_syscall<int>(::remove(pathname.c_str())); })
                    .then([pathname = sstring(pathname)](syscall_result<int> sr) {
                        sr.throw_fs_exception_if_error("remove failed", pathname);
                        return make_ready_future<>();
                    });
            });
        }

        future<> reactor::rename_file(std::string_view old_pathname, std::string_view new_pathname) noexcept {
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([old_pathname, new_pathname] {
                return engine()
                    ._thread_pool
                    ->submit<syscall_result<int>>(
                        [old_pathname = sstring(old_pathname), new_pathname = sstring(new_pathname)] {
                            return wrap_syscall<int>(::rename(old_pathname.c_str(), new_pathname.c_str()));
                        })
                    .then([old_pathname = sstring(old_pathname),
                           new_pathname = sstring(new_pathname)](syscall_result<int> sr) {
                        sr.throw_fs_exception_if_error("rename failed", old_pathname, new_pathname);
                        return make_ready_future<>();
                    });
            });
        }

        future<> reactor::link_file(std::string_view oldpath, std::string_view newpath) noexcept {
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([oldpath, newpath] {
                return engine()
                    ._thread_pool
                    ->submit<syscall_result<int>>([oldpath = sstring(oldpath), newpath = sstring(newpath)] {
                        return wrap_syscall<int>(::link(oldpath.c_str(), newpath.c_str()));
                    })
                    .then([oldpath = sstring(oldpath), newpath = sstring(newpath)](syscall_result<int> sr) {
                        sr.throw_fs_exception_if_error("link failed", oldpath, newpath);
                        return make_ready_future<>();
                    });
            });
        }

        future<> reactor::chmod(std::string_view name, file_permissions permissions) noexcept {
            auto mode = static_cast<mode_t>(permissions);
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([name, mode, this] {
                return _thread_pool
                    ->submit<syscall_result<int>>(
                        [name = sstring(name), mode] { return wrap_syscall<int>(::chmod(name.c_str(), mode)); })
                    .then([name = sstring(name), mode](syscall_result<int> sr) {
                        if (sr.result == -1) {
                            auto reason = format("chmod(0{:o}) failed", mode);
                            sr.throw_fs_exception(reason, boost::filesystem::path(name.c_str()));
                        }
                        return make_ready_future<>();
                    });
            });
        }

#if BOOST_OS_LINUX
        directory_entry_type stat_to_entry_type(__mode_t type) {
#elif BOOST_OS_MACOS || BOOST_OS_IOS
        directory_entry_type stat_to_entry_type(mode_t type) {
#endif
            if (S_ISDIR(type)) {
                return directory_entry_type::directory;
            }
            if (S_ISBLK(type)) {
                return directory_entry_type::block_device;
            }
            if (S_ISCHR(type)) {
                return directory_entry_type::char_device;
            }
            if (S_ISFIFO(type)) {
                return directory_entry_type::fifo;
            }
            if (S_ISLNK(type)) {
                return directory_entry_type::link;
            }
            if (S_ISSOCK(type)) {
                return directory_entry_type::socket;
            }
            if (S_ISREG(type)) {
                return directory_entry_type::regular;
            }
            return directory_entry_type::unknown;
        }

        future<boost::optional<directory_entry_type>> reactor::file_type(std::string_view name,
                                                                         follow_symlink follow) noexcept {
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([name, follow, this] {
                return _thread_pool
                    ->submit<syscall_result_extra<struct stat>>([name = sstring(name), follow] {
                        struct stat st;
                        auto stat_syscall = follow ? stat : lstat;
                        auto ret = stat_syscall(name.c_str(), &st);
                        return wrap_syscall(ret, st);
                    })
                    .then([name = sstring(name)](syscall_result_extra<struct stat> sr) {
                        if (long(sr.result) == -1) {
                            if (sr.error != ENOENT && sr.error != ENOTDIR) {
                                sr.throw_fs_exception_if_error("stat failed", name);
                            }
                            return make_ready_future<boost::optional<directory_entry_type>>(
                                boost::optional<directory_entry_type>());
                        }
                        return make_ready_future<boost::optional<directory_entry_type>>(
                            boost::optional<directory_entry_type>(stat_to_entry_type(sr.extra.st_mode)));
                    });
            });
        }

        future<boost::optional<directory_entry_type>> file_type(std::string_view name, follow_symlink follow) noexcept {
            return engine().file_type(name, follow);
        }

        static std::chrono::system_clock::time_point timespec_to_time_point(const timespec &ts) {
            auto d = std::chrono::duration_cast<std::chrono::system_clock::duration>(ts.tv_sec * 1s + ts.tv_nsec * 1ns);
            return std::chrono::system_clock::time_point(d);
        }

        future<struct stat> reactor::fstat(int fd) noexcept {
            return _thread_pool
                ->submit<syscall_result_extra<struct stat>>([fd] {
                    struct stat st;
                    auto ret = ::fstat(fd, &st);
                    return wrap_syscall(ret, st);
                })
                .then([](syscall_result_extra<struct stat> ret) {
                    ret.throw_if_error();
                    return make_ready_future<struct stat>(ret.extra);
                });
        }

        future<stat_data> reactor::file_stat(std::string_view pathname, follow_symlink follow) noexcept {
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([pathname, follow, this] {
                return _thread_pool
                    ->submit<syscall_result_extra<struct stat>>([pathname = sstring(pathname), follow] {
                        struct stat st;
                        auto stat_syscall = follow ? stat : lstat;
                        auto ret = stat_syscall(pathname.c_str(), &st);
                        return wrap_syscall(ret, st);
                    })
                    .then([pathname = sstring(pathname)](syscall_result_extra<struct stat> sr) {
                        sr.throw_fs_exception_if_error("stat failed", pathname);
                        struct stat &st = sr.extra;
                        stat_data sd;
                        sd.device_id = st.st_dev;
                        sd.inode_number = st.st_ino;
                        sd.mode = st.st_mode;
                        sd.type = stat_to_entry_type(st.st_mode);
                        sd.number_of_links = st.st_nlink;
                        sd.uid = st.st_uid;
                        sd.gid = st.st_gid;
                        sd.rdev = st.st_rdev;
                        sd.size = st.st_size;
                        sd.block_size = st.st_blksize;
                        sd.allocated_size = st.st_blocks * 512UL;
#if BOOST_OS_LINUX
                        sd.time_accessed = timespec_to_time_point(st.st_atim);
                        sd.time_modified = timespec_to_time_point(st.st_mtim);
                        sd.time_changed = timespec_to_time_point(st.st_ctim);
#elif BOOST_OS_MACOS || BOOST_OS_IOS
                        sd.time_accessed = timespec_to_time_point(st.st_atimespec);
                        sd.time_modified = timespec_to_time_point(st.st_mtimespec);
                        sd.time_changed = timespec_to_time_point(st.st_ctimespec);

#endif
                        return make_ready_future<stat_data>(std::move(sd));
                    });
            });
        }

        future<uint64_t> reactor::file_size(std::string_view pathname) noexcept {
            return file_stat(pathname, follow_symlink::yes).then([](stat_data sd) {
                return make_ready_future<uint64_t>(sd.size);
            });
        }

        future<bool> reactor::file_accessible(std::string_view pathname, access_flags flags) noexcept {
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([pathname, flags, this] {
                return _thread_pool
                    ->submit<syscall_result<int>>([pathname = sstring(pathname), flags] {
                        auto aflags = std::underlying_type_t<access_flags>(flags);
                        auto ret = ::access(pathname.c_str(), aflags);
                        return wrap_syscall(ret);
                    })
                    .then([pathname = sstring(pathname), flags](syscall_result<int> sr) {
                        if (sr.result < 0) {
                            if ((sr.error == ENOENT && flags == access_flags::exists) ||
                                (sr.error == EACCES && flags != access_flags::exists)) {
                                return make_ready_future<bool>(false);
                            }
                            sr.throw_fs_exception("access failed", boost::filesystem::path(pathname.c_str()));
                        }

                        return make_ready_future<bool>(true);
                    });
            });
        }

        future<fs_type> reactor::file_system_at(std::string_view pathname) noexcept {
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([pathname, this] {
                return _thread_pool
                    ->submit<syscall_result_extra<struct statfs>>([pathname = sstring(pathname)] {
                        struct statfs st;
                        auto ret = statfs(pathname.c_str(), &st);
                        return wrap_syscall(ret, st);
                    })
                    .then([pathname = sstring(pathname)](syscall_result_extra<struct statfs> sr) {
                        static std::unordered_map<long int, fs_type> type_mapper = {
                            {0x58465342, fs_type::xfs},
#if BOOST_OS_LINUX
                            {EXT2_SUPER_MAGIC, fs_type::ext2},
                            {EXT3_SUPER_MAGIC, fs_type::ext3},
                            {EXT4_SUPER_MAGIC, fs_type::ext4},
                            {BTRFS_SUPER_MAGIC, fs_type::btrfs},
                            {TMPFS_MAGIC, fs_type::tmpfs},
#endif
                            {0x4244, fs_type::hfs}
                        };
                        sr.throw_fs_exception_if_error("statfs failed", pathname);

                        fs_type ret = fs_type::other;
                        if (type_mapper.count(sr.extra.f_type) != 0) {
                            ret = type_mapper.at(sr.extra.f_type);
                        }
                        return make_ready_future<fs_type>(ret);
                    });
            });
        }

        future<struct statfs> reactor::fstatfs(int fd) noexcept {
            return _thread_pool
                ->submit<syscall_result_extra<struct statfs>>([fd] {
                    struct statfs st;
                    auto ret = ::fstatfs(fd, &st);
                    return wrap_syscall(ret, st);
                })
                .then([](syscall_result_extra<struct statfs> sr) {
                    sr.throw_if_error();
                    struct statfs st = sr.extra;
                    return make_ready_future<struct statfs>(std::move(st));
                });
        }

        future<struct statvfs> reactor::statvfs(std::string_view pathname) noexcept {
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([pathname, this] {
                return _thread_pool
                    ->submit<syscall_result_extra<struct statvfs>>([pathname = sstring(pathname)] {
                        struct statvfs st;
                        auto ret = ::statvfs(pathname.c_str(), &st);
                        return wrap_syscall(ret, st);
                    })
                    .then([pathname = sstring(pathname)](syscall_result_extra<struct statvfs> sr) {
                        sr.throw_fs_exception_if_error("statvfs failed", pathname);
                        struct statvfs st = sr.extra;
                        return make_ready_future<struct statvfs>(std::move(st));
                    });
            });
        }

        future<file> reactor::open_directory(std::string_view name) noexcept {
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([name, this] {
                auto oflags = O_DIRECTORY | O_CLOEXEC | O_RDONLY;
                return _thread_pool
                    ->submit<syscall_result<int>>(
                        [name = sstring(name), oflags] { return wrap_syscall<int>(::open(name.c_str(), oflags)); })
                    .then([name = sstring(name), oflags](syscall_result<int> sr) {
                        sr.throw_fs_exception_if_error("open failed", name);
                        return make_file_impl(sr.result, file_open_options(), oflags);
                    })
                    .then(
                        [](shared_ptr<file_impl> file_impl) { return make_ready_future<file>(std::move(file_impl)); });
            });
        }

        future<> reactor::make_directory(std::string_view name, file_permissions permissions) noexcept {
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([name, permissions, this] {
                return _thread_pool
                    ->submit<syscall_result<int>>([name = sstring(name), permissions] {
                        auto mode = static_cast<mode_t>(permissions);
                        return wrap_syscall<int>(::mkdir(name.c_str(), mode));
                    })
                    .then([name = sstring(name)](syscall_result<int> sr) {
                        sr.throw_fs_exception_if_error("mkdir failed", name);
                    });
            });
        }

        future<> reactor::touch_directory(std::string_view name, file_permissions permissions) noexcept {
            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([name, permissions] {
                return engine()
                    ._thread_pool
                    ->submit<syscall_result<int>>([name = sstring(name), permissions] {
                        auto mode = static_cast<mode_t>(permissions);
                        return wrap_syscall<int>(::mkdir(name.c_str(), mode));
                    })
                    .then([name = sstring(name)](syscall_result<int> sr) {
                        if (sr.result == -1 && sr.error != EEXIST) {
                            sr.throw_fs_exception("mkdir failed", boost::filesystem::path(name.c_str()));
                        }
                        return make_ready_future<>();
                    });
            });
        }

        future<> reactor::fdatasync(int fd) noexcept {
            ++_fsyncs;
            if (_bypass_fsync) {
                return make_ready_future<>();
            }
            if (_have_aio_fsync) {
                // Does not go through the I/O queue, but has to be deleted
                struct fsync_io_desc final : public io_completion {
                    promise<> _pr;

                public:
                    virtual void complete(size_t res) noexcept override {
                        _pr.set_value();
                        delete this;
                    }

                    virtual void set_exception(std::exception_ptr eptr) noexcept override {
                        _pr.set_exception(std::move(eptr));
                        delete this;
                    }

                    future<> get_future() {
                        return _pr.get_future();
                    }
                };

                return futurize_invoke([this, fd] {
                    auto desc = new fsync_io_desc;
                    auto fut = desc->get_future();
                    auto req = io_request::make_fdatasync(fd);
                    _io_sink.submit(desc, std::move(req));
                    return fut;
                });
            }
            return _thread_pool
                ->submit<syscall_result<int>>([fd] {
#if BOOST_OS_LINUX
                    return wrap_syscall<int>(::fdatasync(fd));
#elif BOOST_OS_MACOS || BOOST_OS_IOS
                    return wrap_syscall<int>(fcntl(fd, F_FULLFSYNC));
#endif
                })
                .then([](syscall_result<int> sr) {
                    sr.throw_if_error();
                    return make_ready_future<>();
                });
        }

        // Note: terminate if arm_highres_timer throws
        // `when` should always be valid
        void reactor::enable_timer(steady_clock_type::time_point when) noexcept {
#ifndef HAVE_OSV
            itimerspec its;
            its.it_interval = {};
            its.it_value = to_timespec(when);
            _backend->arm_highres_timer(its);
#else
            using ns = std::chrono::nanoseconds;
            WITH_LOCK(_timer_mutex) {
                _timer_due = std::chrono::duration_cast<ns>(when.time_since_epoch()).count();
                _timer_cond.wake_one();
            }
#endif
        }

        void reactor::add_timer(timer<steady_clock_type> *tmr) noexcept {
            if (queue_timer(tmr)) {
                enable_timer(_timers.get_next_timeout());
            }
        }

        bool reactor::queue_timer(timer<steady_clock_type> *tmr) noexcept {
            return _timers.insert(*tmr);
        }

        void reactor::del_timer(timer<steady_clock_type> *tmr) noexcept {
            if (tmr->_expired) {
                _expired_timers.erase(_expired_timers.iterator_to(*tmr));
                tmr->_expired = false;
            } else {
                _timers.remove(*tmr);
            }
        }

        void reactor::add_timer(timer<lowres_clock> *tmr) noexcept {
            if (queue_timer(tmr)) {
                _lowres_next_timeout = _lowres_timers.get_next_timeout();
            }
        }

        bool reactor::queue_timer(timer<lowres_clock> *tmr) noexcept {
            return _lowres_timers.insert(*tmr);
        }

        void reactor::del_timer(timer<lowres_clock> *tmr) noexcept {
            if (tmr->_expired) {
                _expired_lowres_timers.erase(_expired_lowres_timers.iterator_to(*tmr));
                tmr->_expired = false;
            } else {
                _lowres_timers.remove(*tmr);
            }
        }

        void reactor::add_timer(timer<manual_clock> *tmr) noexcept {
            queue_timer(tmr);
        }

        bool reactor::queue_timer(timer<manual_clock> *tmr) noexcept {
            return _manual_timers.insert(*tmr);
        }

        void reactor::del_timer(timer<manual_clock> *tmr) noexcept {
            if (tmr->_expired) {
                _expired_manual_timers.erase(_expired_manual_timers.iterator_to(*tmr));
                tmr->_expired = false;
            } else {
                _manual_timers.remove(*tmr);
            }
        }

        void reactor::at_exit(noncopyable_function<future<>()> func) {
            assert(!_stopping);
            _exit_funcs.push_back(std::move(func));
        }

        future<> reactor::run_exit_tasks() {
            _stop_requested.broadcast();
            _stopping = true;
            stop_aio_eventfd_loop();
            return do_for_each(_exit_funcs.rbegin(), _exit_funcs.rend(), [](auto &func) { return func(); });
        }

        void reactor::stop() {
            assert(_id == 0);
            smp::cleanup_cpu();
            if (!_stopping) {
                // Run exit tasks locally and then stop all other engines
                // in the background and wait on semaphore for all to complete.
                // Finally, set _stopped on cpu 0.
                (void)run_exit_tasks().then([this] {
                    return do_with(semaphore(0), [this](semaphore &sem) {
                        // Stop other cpus asynchronously, signal when done.
                        (void)smp::invoke_on_others(0, [] {
                            smp::cleanup_cpu();
                            return engine().run_exit_tasks().then([] { engine()._stopped = true; });
                        }).then([&sem]() { sem.signal(); });
                        return sem.wait().then([this] { _stopped = true; });
                    });
                });
            }
        }

        void reactor::exit(int ret) {
            // Run stop() asynchronously on cpu 0.
            (void)smp::submit_to(0, [this, ret] {
                _return = ret;
                stop();
            });
        }

        uint64_t reactor::pending_task_count() const {
            uint64_t ret = 0;
            for (auto &&tq : _task_queues) {
                ret += tq->_q.size();
            }
            return ret;
        }

        uint64_t reactor::tasks_processed() const {
            return _global_tasks_processed;
        }

        void reactor::register_metrics() {

            namespace sm = nil::actor::metrics;

            _metric_groups.add_group(
                "reactor",
                {
                    sm::make_gauge("tasks_pending", std::bind(&reactor::pending_task_count, this),
                                   sm::description("Number of pending tasks in the queue")),
                    // total_operations value:DERIVE:0:U
                    sm::make_derive("tasks_processed", std::bind(&reactor::tasks_processed, this),
                                    sm::description("Total tasks processed")),
                    sm::make_derive("polls", _polls, sm::description("Number of times pollers were executed")),
                    sm::make_derive("timers_pending", std::bind(&decltype(_timers)::size, &_timers),
                                    sm::description("Number of tasks in the timer-pending queue")),
                    sm::make_gauge(
                        "utilization", [this] { return (1 - _load) * 100; }, sm::description("CPU utilization")),
                    sm::make_derive(
                        "cpu_busy_ms", [this]() -> int64_t { return total_busy_time() / 1ms; },
                        sm::description("Total cpu busy time in milliseconds")),
                    sm::make_derive(
                        "cpu_steal_time_ms", [this]() -> int64_t { return total_steal_time() / 1ms; },
                        sm::description(
                            "Total steal time, the time in which some other process was running while =nil; Actor "
                            "was not trying to run (not sleeping)."
                            "Because this is in userspace, some time that could be legitimally thought as "
                            "steal time is not accounted as such. For example, if we are sleeping and can wake "
                            "up but the kernel hasn't woken us up yet.")),
                    // total_operations value:DERIVE:0:U
                    sm::make_derive("aio_reads", _io_stats.aio_reads, sm::description("Total aio-reads operations")),

                    sm::make_total_bytes("aio_bytes_read", _io_stats.aio_read_bytes,
                                         sm::description("Total aio-reads bytes")),
                    // total_operations value:DERIVE:0:U
                    sm::make_derive("aio_writes", _io_stats.aio_writes, sm::description("Total aio-writes operations")),
                    sm::make_total_bytes("aio_bytes_write", _io_stats.aio_write_bytes,
                                         sm::description("Total aio-writes bytes")),
                    sm::make_derive("aio_errors", _io_stats.aio_errors, sm::description("Total aio errors")),
                    // total_operations value:DERIVE:0:U
                    sm::make_derive("fsyncs", _fsyncs, sm::description("Total number of fsync operations")),
                    // total_operations value:DERIVE:0:U
                    sm::make_derive("io_threaded_fallbacks",
                                    std::bind(&thread_pool::operation_count, _thread_pool.get()),
                                    sm::description("Total number of io-threaded-fallbacks operations")),

                });

            _metric_groups.add_group("memory",
                                     {sm::make_derive(
                                          "malloc_operations", [] { return memory::stats().mallocs(); },
                                          sm::description("Total number of malloc operations")),
                                      sm::make_derive(
                                          "free_operations", [] { return memory::stats().frees(); },
                                          sm::description("Total number of free operations")),
                                      sm::make_derive(
                                          "cross_cpu_free_operations", [] { return memory::stats().cross_cpu_frees(); },
                                          sm::description("Total number of cross cpu free")),
                                      sm::make_gauge(
                                          "malloc_live_objects", [] { return memory::stats().live_objects(); },
                                          sm::description("Number of live objects")),
                                      sm::make_current_bytes(
                                          "free_memory", [] { return memory::stats().free_memory(); },
                                          sm::description("Free memeory size in bytes")),
                                      sm::make_current_bytes(
                                          "total_memory", [] { return memory::stats().total_memory(); },
                                          sm::description("Total memeory size in bytes")),
                                      sm::make_current_bytes(
                                          "allocated_memory", [] { return memory::stats().allocated_memory(); },
                                          sm::description("Allocated memeory size in bytes")),
                                      sm::make_derive(
                                          "reclaims_operations", [] { return memory::stats().reclaims(); },
                                          sm::description("Total reclaims operations"))});

            _metric_groups.add_group(
                "reactor",
                {
                    sm::make_derive(
                        "logging_failures", [] { return logging_failures; },
                        sm::description("Total number of logging failures")),
                    // total_operations value:DERIVE:0:U
                    sm::make_derive("cpp_exceptions", _cxx_exceptions,
                                    sm::description("Total number of C++ exceptions")),
                    sm::make_derive("abandoned_failed_futures", _abandoned_failed_futures,
                                    sm::description("Total number of abandoned failed futures, futures destroyed while "
                                                    "still containing an exception")),
                });

            using namespace nil::actor::metrics;
            _metric_groups.add_group(
                "reactor",
                {
                    make_counter(
                        "fstream_reads", _io_stats.fstream_reads,
                        description("Counts reads from disk file streams.  A high rate indicates high disk activity."
                                    " Contrast with other fstream_read* counters to locate bottlenecks.")),
                    make_derive(
                        "fstream_read_bytes", _io_stats.fstream_read_bytes,
                        description(
                            "Counts bytes read from disk file streams.  A high rate indicates high disk activity."
                            " Divide by fstream_reads to determine average read size.")),
                    make_counter("fstream_reads_blocked", _io_stats.fstream_reads_blocked,
                                 description("Counts the number of times a disk read could not be satisfied from "
                                             "read-ahead buffers, and had to block."
                                             " Indicates short streams, or incorrect read ahead configuration.")),
                    make_derive(
                        "fstream_read_bytes_blocked", _io_stats.fstream_read_bytes_blocked,
                        description("Counts the number of bytes read from disk that could not be satisfied from "
                                    "read-ahead buffers, and had to block."
                                    " Indicates short streams, or incorrect read ahead configuration.")),
                    make_counter("fstream_reads_aheads_discarded", _io_stats.fstream_read_aheads_discarded,
                                 description("Counts the number of times a buffer that was read ahead of time and was "
                                             "discarded because it was not needed, wasting disk bandwidth."
                                             " Indicates over-eager read ahead configuration.")),
                    make_derive("fstream_reads_ahead_bytes_discarded", _io_stats.fstream_read_ahead_discarded_bytes,
                                description("Counts the number of buffered bytes that were read ahead of time and were "
                                            "discarded because they were not needed, wasting disk bandwidth."
                                            " Indicates over-eager read ahead configuration.")),
                });
        }

        void reactor::run_tasks(task_queue &tq) {
            // Make sure new tasks will inherit our scheduling group
            *detail::current_scheduling_group_ptr() = scheduling_group(tq._id);
            auto &tasks = tq._q;
            while (!tasks.empty()) {
                auto tsk = tasks.front();
                tasks.pop_front();
#if BOOST_OS_LINUX
                STAP_PROBE(actor, reactor_run_tasks_single_start);
#endif
                task_histogram_add_task(*tsk);
                _current_task = tsk;
                tsk->run_and_dispose();
                _current_task = nullptr;
#if BOOST_OS_LINUX
                STAP_PROBE(actor, reactor_run_tasks_single_end);
#endif
                ++tq._tasks_processed;
                ++_global_tasks_processed;
                // check at end of loop, to allow at least one task to run
                if (need_preempt()) {
                    if (tasks.size() <= _max_task_backlog) {
                        break;
                    } else {
                        // While need_preempt() is set, task execution is inefficient due to
                        // need_preempt() checks breaking out of loops and .then() calls. See
                        // #302.
                        reset_preemption_monitor();
                    }
                }
            }
        }

#ifdef ACTOR_SHUFFLE_TASK_QUEUE
        void reactor::shuffle(task *&t, task_queue &q) {
            static thread_local std::mt19937 gen = std::mt19937(std::default_random_engine()());
            std::uniform_int_distribution<size_t> tasks_dist {0, q._q.size() - 1};
            auto &to_swap = q._q[tasks_dist(gen)];
            std::swap(to_swap, t);
        }
#endif

        void reactor::force_poll() {
            request_preemption();
        }

        bool reactor::flush_tcp_batches() {
            bool work = !_flush_batching.empty();
            while (!_flush_batching.empty()) {
                auto os = std::move(_flush_batching.front());
                _flush_batching.pop_front();
                os->poll_flush();
            }
            return work;
        }

        bool reactor::do_expire_lowres_timers() noexcept {
            if (_lowres_next_timeout == lowres_clock::time_point()) {
                return false;
            }
            auto now = lowres_clock::now();
            if (now >= _lowres_next_timeout) {
                complete_timers(_lowres_timers, _expired_lowres_timers, [this]() noexcept {
                    if (!_lowres_timers.empty()) {
                        _lowres_next_timeout = _lowres_timers.get_next_timeout();
                    } else {
                        _lowres_next_timeout = lowres_clock::time_point();
                    }
                });
                return true;
            }
            return false;
        }

        void reactor::expire_manual_timers() noexcept {
            complete_timers(_manual_timers, _expired_manual_timers, []() noexcept {});
        }

        void manual_clock::expire_timers() noexcept {
            local_engine->expire_manual_timers();
        }

        void manual_clock::advance(manual_clock::duration d) noexcept {
            _now.fetch_add(d.count());
            if (local_engine) {
                schedule_urgent(make_task(default_scheduling_group(), &manual_clock::expire_timers));
                // Expire timers on all cores in the background.
                (void)smp::invoke_on_all(&manual_clock::expire_timers);
            }
        }

        bool reactor::do_check_lowres_timers() const noexcept {
            if (_lowres_next_timeout == lowres_clock::time_point()) {
                return false;
            }
            return lowres_clock::now() > _lowres_next_timeout;
        }

#ifndef HAVE_OSV

        class reactor::kernel_submit_work_pollfn final : public simple_pollfn<true> {
            reactor &_r;

        public:
            kernel_submit_work_pollfn(reactor &r) : _r(r) {
            }
            virtual bool poll() override final {
                return _r._backend->kernel_submit_work();
            }
        };

#endif

        class reactor::signal_pollfn final : public reactor::pollfn {
            reactor &_r;

        public:
            signal_pollfn(reactor &r) : _r(r) {
            }
            virtual bool poll() final override {
                return _r._signals.poll_signal();
            }
            virtual bool pure_poll() override final {
                return _r._signals.pure_poll_signal();
            }
            virtual bool try_enter_interrupt_mode() override {
                // Signals will interrupt our epoll_pwait() call, but
                // disable them now to avoid a signal between this point
                // and epoll_pwait()
                sigset_t block_all;
                sigfillset(&block_all);
                ::pthread_sigmask(SIG_SETMASK, &block_all, &_r._active_sigmask);
                if (poll()) {
                    // raced already, and lost
                    exit_interrupt_mode();
                    return false;
                }
                return true;
            }
            virtual void exit_interrupt_mode() override final {
                ::pthread_sigmask(SIG_SETMASK, &_r._active_sigmask, nullptr);
            }
        };

        class reactor::batch_flush_pollfn final : public simple_pollfn<true> {
            reactor &_r;

        public:
            batch_flush_pollfn(reactor &r) : _r(r) {
            }
            virtual bool poll() final override {
                return _r.flush_tcp_batches();
            }
        };

        class reactor::reap_kernel_completions_pollfn final : public reactor::pollfn {
            reactor &_r;

        public:
            reap_kernel_completions_pollfn(reactor &r) : _r(r) {
            }
            virtual bool poll() final override {
                return _r.reap_kernel_completions();
            }
            virtual bool pure_poll() override final {
                return poll();    // actually performs work, but triggers no user continuations, so okay
            }
            virtual bool try_enter_interrupt_mode() override {
                return _r._backend->kernel_events_can_sleep();
            }
            virtual void exit_interrupt_mode() override final {
            }
        };

        class reactor::io_queue_submission_pollfn final : public reactor::pollfn {
            reactor &_r;
            // Wake-up the reactor with highres timer when the io-queue
            // decides to delay dispatching until some time point in
            // the future
            timer<> _nearest_wakeup {[this] { _armed = false; }};
            bool _armed = false;

        public:
            io_queue_submission_pollfn(reactor &r) : _r(r) {
            }
            virtual bool poll() final override {
                return _r.flush_pending_aio();
            }
            virtual bool pure_poll() override final {
                return poll();
            }
            virtual bool try_enter_interrupt_mode() override {
                auto next = _r.next_pending_aio();
                auto now = steady_clock_type::now();
                if (next <= now) {
                    return false;
                }
                _nearest_wakeup.arm(next);
                _armed = true;
                return true;
            }
            virtual void exit_interrupt_mode() override final {
                if (_armed) {
                    _nearest_wakeup.cancel();
                    _armed = false;
                }
            }
        };

        // Other cpus can queue items for us to free; and they won't notify
        // us about them.  But it's okay to ignore those items, freeing them
        // doesn't have any side effects.
        //
        // We'll take care of those items when we wake up for another reason.
        class reactor::drain_cross_cpu_freelist_pollfn final : public simple_pollfn<true> {
        public:
            virtual bool poll() final override {
                return memory::drain_cross_cpu_freelist();
            }
        };

        class reactor::lowres_timer_pollfn final : public reactor::pollfn {
            reactor &_r;
            // A highres timer is implemented as a waking  signal; so
            // we arm one when we have a lowres timer during sleep, so
            // it can wake us up.
            timer<> _nearest_wakeup {[this] { _armed = false; }};
            bool _armed = false;

        public:
            lowres_timer_pollfn(reactor &r) : _r(r) {
            }
            virtual bool poll() final override {
                return _r.do_expire_lowres_timers();
            }
            virtual bool pure_poll() final override {
                return _r.do_check_lowres_timers();
            }
            virtual bool try_enter_interrupt_mode() override {
                // arm our highres timer so a signal will wake us up
                auto next = _r._lowres_next_timeout;
                if (next == lowres_clock::time_point()) {
                    // no pending timers
                    return true;
                }
                auto now = lowres_clock::now();
                if (next <= now) {
                    // whoops, go back
                    return false;
                }
                _nearest_wakeup.arm(next - now);
                _armed = true;
                return true;
            }
            virtual void exit_interrupt_mode() override final {
                if (_armed) {
                    _nearest_wakeup.cancel();
                    _armed = false;
                }
            }
        };

        class reactor::smp_pollfn final : public reactor::pollfn {
            reactor &_r;

        public:
            smp_pollfn(reactor &r) : _r(r) {
            }
            virtual bool poll() final override {
                return (smp::poll_queues() || alien::smp::poll_queues());
            }
            virtual bool pure_poll() final override {
                return (smp::pure_poll_queues() || alien::smp::pure_poll_queues());
            }
            virtual bool try_enter_interrupt_mode() override {
                // systemwide_memory_barrier() is very slow if run concurrently,
                // so don't go to sleep if it is running now.
                _r._sleeping.store(true, std::memory_order_relaxed);
                bool barrier_done = try_systemwide_memory_barrier();
                if (!barrier_done) {
                    _r._sleeping.store(false, std::memory_order_relaxed);
                    return false;
                }
                if (poll()) {
                    // raced
                    _r._sleeping.store(false, std::memory_order_relaxed);
                    return false;
                }
                return true;
            }
            virtual void exit_interrupt_mode() override final {
                _r._sleeping.store(false, std::memory_order_relaxed);
            }
        };

        class reactor::execution_stage_pollfn final : public reactor::pollfn {
            detail::execution_stage_manager &_esm;

        public:
            execution_stage_pollfn() : _esm(detail::execution_stage_manager::get()) {
            }

            virtual bool poll() override {
                return _esm.flush();
            }
            virtual bool pure_poll() override {
                return _esm.poll();
            }
            virtual bool try_enter_interrupt_mode() override {
                // This is a passive poller, so if a previous poll
                // returned false (idle), there's no more work to do.
                return true;
            }
            virtual void exit_interrupt_mode() override {
            }
        };

        class reactor::syscall_pollfn final : public reactor::pollfn {
            reactor &_r;

        public:
            syscall_pollfn(reactor &r) : _r(r) {
            }
            virtual bool poll() final override {
                return _r._thread_pool->complete();
            }
            virtual bool pure_poll() override final {
                return poll();    // actually performs work, but triggers no user continuations, so okay
            }
            virtual bool try_enter_interrupt_mode() override {
                _r._thread_pool->enter_interrupt_mode();
                if (poll()) {
                    // raced
                    _r._thread_pool->exit_interrupt_mode();
                    return false;
                }
                return true;
            }
            virtual void exit_interrupt_mode() override final {
                _r._thread_pool->exit_interrupt_mode();
            }
        };

        void reactor::wakeup() {
            uint64_t one = 1;
#if BOOST_OS_LINUX
            ::write(_notify_eventfd.get(), &one, sizeof(one));
#else
            epoll_shim_write(_notify_eventfd.get(), &one, sizeof(one));
#endif
        }

        void reactor::start_aio_eventfd_loop() {
            if (!_aio_eventfd) {
                return;
            }
            future<> loop_done = repeat([this] {
                return _aio_eventfd->readable().then([this] {
                    char garbage[8];
#if BOOST_OS_LINUX
                    ::read(_aio_eventfd->get_fd(), garbage, 8);    // totally uninteresting
#else
                    epoll_shim_read(_aio_eventfd->get_fd(), garbage, 8);    // totally uninteresting
#endif
                    return _stopping ? stop_iteration::yes : stop_iteration::no;
                });
            });
            // must use make_lw_shared, because at_exit expects a copyable function
            at_exit([loop_done = make_lw_shared(std::move(loop_done))] { return std::move(*loop_done); });
        }

        void reactor::stop_aio_eventfd_loop() {
            if (!_aio_eventfd) {
                return;
            }
            uint64_t one = 1;
#if BOOST_OS_LINUX
            ::write(_aio_eventfd->get_fd(), &one, 8);
#else
            epoll_shim_write(_aio_eventfd->get_fd(), &one, 8);
#endif
        }

        inline bool reactor::have_more_tasks() const {
            return _active_task_queues.size() + _activating_task_queues.size();
        }

        void reactor::insert_active_task_queue(task_queue *tq) {
            tq->_active = true;
            auto &atq = _active_task_queues;
            auto less = task_queue::indirect_compare();
            if (atq.empty() || less(atq.back(), tq)) {
                // Common case: idle->working
                // Common case: CPU intensive task queue going to the back
                atq.push_back(tq);
            } else {
                // Common case: newly activated queue preempting everything else
                atq.push_front(tq);
                // Less common case: newly activated queue behind something already active
                size_t i = 0;
                while (i + 1 != atq.size() && !less(atq[i], atq[i + 1])) {
                    std::swap(atq[i], atq[i + 1]);
                    ++i;
                }
            }
        }

        reactor::task_queue *reactor::pop_active_task_queue(sched_clock::time_point now) {
            task_queue *tq = _active_task_queues.front();
            _active_task_queues.pop_front();
            tq->_starvetime += now - tq->_ts;
            return tq;
        }

        void reactor::insert_activating_task_queues() {
            // Quadratic, but since we expect the common cases in insert_active_task_queue() to dominate, faster
            for (auto &&tq : _activating_task_queues) {
                insert_active_task_queue(tq);
            }
            _activating_task_queues.clear();
        }

        void reactor::run_some_tasks() {
            if (!have_more_tasks()) {
                return;
            }
            sched_print("run_some_tasks: start");
            reset_preemption_monitor();

            sched_clock::time_point t_run_completed = std::chrono::steady_clock::now();
#if BOOST_OS_LINUX
            STAP_PROBE(actor, reactor_run_tasks_start);
#endif
            _cpu_stall_detector->start_task_run(t_run_completed);
            do {
                auto t_run_started = t_run_completed;
                insert_activating_task_queues();
                task_queue *tq = pop_active_task_queue(t_run_started);
                sched_print("running tq {} {}", (void *)tq, tq->_name);
                tq->_current = true;
                _last_vruntime = std::max(tq->_vruntime, _last_vruntime);
                run_tasks(*tq);
                tq->_current = false;
                t_run_completed = std::chrono::steady_clock::now();
                auto delta = t_run_completed - t_run_started;
                account_runtime(*tq, delta);
                sched_print("run complete ({} {}); time consumed {} usec; final vruntime {} empty {}", (void *)tq,
                            tq->_name, delta / 1us, tq->_vruntime, tq->_q.empty());
                tq->_ts = t_run_completed;
                if (!tq->_q.empty()) {
                    insert_active_task_queue(tq);
                } else {
                    tq->_active = false;
                }
            } while (have_more_tasks() && !need_preempt());
            _cpu_stall_detector->end_task_run(t_run_completed);
#if BOOST_OS_LINUX
            STAP_PROBE(actor, reactor_run_tasks_end);
#endif
            *detail::current_scheduling_group_ptr() =
                default_scheduling_group();    // Prevent inheritance from last group run
            sched_print("run_some_tasks: end");
        }

        void reactor::activate(task_queue &tq) {
            if (tq._active) {
                return;
            }
            sched_print("activating {} {}", (void *)&tq, tq._name);
            // If activate() was called, the task queue is likely network-bound or I/O bound, not CPU-bound. As
            // such its vruntime will be low, and it will have a large advantage over other task queues. Limit
            // the advantage so it doesn't dominate scheduling for a long time, in case it _does_ become CPU
            // bound later.
            //
            // FIXME: different scheduling groups have different sensitivity to jitter, take advantage
            if (_last_vruntime > tq._vruntime) {
                sched_print("tq {} {} losing vruntime {} due to sleep", (void *)&tq, tq._name,
                            _last_vruntime - tq._vruntime);
            }
            tq._vruntime = std::max(_last_vruntime, tq._vruntime);
            auto now = std::chrono::steady_clock::now();
            tq._waittime += now - tq._ts;
            tq._ts = now;
            _activating_task_queues.push_back(&tq);
        }

        void reactor::service_highres_timer() noexcept {
            complete_timers(_timers, _expired_timers, [this]() noexcept {
                if (!_timers.empty()) {
                    enable_timer(_timers.get_next_timeout());
                }
            });
        }

        int reactor::run() {
#ifndef ACTOR_ASAN_ENABLED
            // SIGSTKSZ is too small when using asan. We also don't need to
            // handle SIGSEGV ourselves when using asan, so just don't install
            // a signal handler stack.
            auto signal_stack = install_signal_handler_stack();
#else
            (void)install_signal_handler_stack;
#endif

            register_metrics();

            // The order in which we execute the pollers is very important for performance.
            //
            // This is because events that are generated in one poller may feed work into others. If
            // they were reversed, we'd only be able to do that work in the next task quota.
            //
            // One example is the relationship between the smp poller and the I/O submission poller:
            // If the smp poller runs first, requests from remote I/O queues can be dispatched right away
            //
            // We will run the pollers in the following order:
            //
            // 1. SMP: any remote event arrives before anything else
            // 2. reap kernel events completion: storage related completions may free up space in the I/O
            //                                   queue.
            // 4. I/O queue: must be after reap, to free up events. If new slots are freed may submit I/O
            // 5. kernel submission: for I/O, will submit what was generated from last step.
            // 6. reap kernel events completion: some of the submissions from last step may return immediately.
            //                                   For example if we are dealing with poll() on a fd that has events.
            poller smp_poller(std::make_unique<smp_pollfn>(*this));

            poller reap_kernel_completions_poller(std::make_unique<reap_kernel_completions_pollfn>(*this));
            poller io_queue_submission_poller(std::make_unique<io_queue_submission_pollfn>(*this));
            poller kernel_submit_work_poller(std::make_unique<kernel_submit_work_pollfn>(*this));
            poller final_real_kernel_completions_poller(std::make_unique<reap_kernel_completions_pollfn>(*this));

            poller batch_flush_poller(std::make_unique<batch_flush_pollfn>(*this));
            poller execution_stage_poller(std::make_unique<execution_stage_pollfn>());

            start_aio_eventfd_loop();

            if (_id == 0 && _cfg.auto_handle_sigint_sigterm) {
                if (_handle_sigint) {
                    _signals.handle_signal_once(SIGINT, [this] { stop(); });
                }
                _signals.handle_signal_once(SIGTERM, [this] { stop(); });
            }

            // Start initialization in the background.
            // Communicate when done using _start_promise.
            (void)_cpu_started.wait(smp::count).then([this] {
                (void)_network_stack->initialize().then([this] { _start_promise.set_value(); });
            });
            // Wait for network stack in the background and then signal all cpus.
            (void)_network_stack_ready->then([this](std::unique_ptr<network_stack> stack) {
                _network_stack = std::move(stack);
                return smp::invoke_on_all([] { engine()._cpu_started.signal(); });
            });

            poller syscall_poller(std::make_unique<syscall_pollfn>(*this));

            poller drain_cross_cpu_freelist(std::make_unique<drain_cross_cpu_freelist_pollfn>());

            poller expire_lowres_timers(std::make_unique<lowres_timer_pollfn>(*this));
            poller sig_poller(std::make_unique<signal_pollfn>(*this));

            using namespace std::chrono_literals;
            timer<lowres_clock> load_timer;
            auto last_idle = _total_idle;
            auto idle_start = sched_clock::now(), idle_end = idle_start;
            load_timer.set_callback([this, &last_idle, &idle_start, &idle_end]() mutable {
                _total_idle += idle_end - idle_start;
                auto load = double((_total_idle - last_idle).count()) /
                            double(std::chrono::duration_cast<sched_clock::duration>(1s).count());
                last_idle = _total_idle;
                load = std::min(load, 1.0);
                idle_start = idle_end;
                _loads.push_front(load);
                if (_loads.size() > 5) {
                    auto drop = _loads.back();
                    _loads.pop_back();
                    _load -= (drop / 5);
                }
                _load += (load / 5);
            });
            load_timer.arm_periodic(1s);

            itimerspec its = nil::actor::posix::to_relative_itimerspec(_task_quota, _task_quota);
            _task_quota_timer.timerfd_settime(0, its);
            auto &task_quote_itimerspec = its;

            struct sigaction sa_block_notifier = {};
            sa_block_notifier.sa_handler = &reactor::block_notifier;
            sa_block_notifier.sa_flags = SA_RESTART;
            auto r = sigaction(cpu_stall_detector::signal_number(), &sa_block_notifier, nullptr);
            assert(r == 0);

            bool idle = false;

            std::function<bool()> check_for_work = [this]() { return poll_once() || have_more_tasks(); };
            std::function<bool()> pure_check_for_work = [this]() { return pure_poll_once() || have_more_tasks(); };
            while (true) {
                run_some_tasks();
                if (_stopped) {
                    load_timer.cancel();
                    // Final tasks may include sending the last response to cpu 0, so run them
                    while (have_more_tasks()) {
                        run_some_tasks();
                    }
                    while (!_at_destroy_tasks->_q.empty()) {
                        run_tasks(*_at_destroy_tasks);
                    }
                    _finished_running_tasks = true;
                    smp::arrive_at_event_loop_end();
                    if (_id == 0) {
                        smp::join_all();
                    }
                    break;
                }

                _polls++;

                if (check_for_work()) {
                    if (idle) {
                        _total_idle += idle_end - idle_start;
                        account_idle(idle_end - idle_start);
                        idle_start = idle_end;
                        idle = false;
                    }
                } else {
                    idle_end = sched_clock::now();
                    if (!idle) {
                        idle_start = idle_end;
                        idle = true;
                    }
                    bool go_to_sleep = true;
                    try {
                        // we can't run check_for_work(), because that can run tasks in the context
                        // of the idle handler which change its state, without the idle handler expecting
                        // it.  So run pure_check_for_work() instead.
                        auto handler_result = _idle_cpu_handler(pure_check_for_work);
                        go_to_sleep = handler_result == idle_cpu_handler_result::no_more_work;
                    } catch (...) {
                        report_exception("Exception while running idle cpu handler", std::current_exception());
                    }
                    if (go_to_sleep) {
                        detail::cpu_relax();
                        if (idle_end - idle_start > _max_poll_time) {
                            // Turn off the task quota timer to avoid spurious wakeups
                            struct itimerspec zero_itimerspec = {};
                            _task_quota_timer.timerfd_settime(0, zero_itimerspec);
                            auto start_sleep = sched_clock::now();
                            _cpu_stall_detector->start_sleep();
                            sleep();
                            _cpu_stall_detector->end_sleep();
                            // We may have slept for a while, so freshen idle_end
                            idle_end = sched_clock::now();
                            _total_sleep += idle_end - start_sleep;
                            _task_quota_timer.timerfd_settime(0, task_quote_itimerspec);
                        }
                    } else {
                        // We previously ran pure_check_for_work(), might not actually have performed
                        // any work.
                        check_for_work();
                    }
                }
            }
            // To prevent ordering issues from rising, destroy the I/O queue explicitly at this point.
            // This is needed because the reactor is destroyed from the thread_local destructors. If
            // the I/O queue happens to use any other infrastructure that is also kept this way (for
            // instance, collectd), we will not have any way to guarantee who is destroyed first.
            _io_queues.clear();
            return _return;
        }

        void reactor::sleep() {
            for (auto i = _pollers.begin(); i != _pollers.end(); ++i) {
                auto ok = (*i)->try_enter_interrupt_mode();
                if (!ok) {
                    while (i != _pollers.begin()) {
                        (*--i)->exit_interrupt_mode();
                    }
                    return;
                }
            }

            _backend->wait_and_process_events(&_active_sigmask);

            for (auto i = _pollers.rbegin(); i != _pollers.rend(); ++i) {
                (*i)->exit_interrupt_mode();
            }
        }

        bool reactor::poll_once() {
            bool work = false;
            for (auto c : _pollers) {
                work |= c->poll();
            }

            return work;
        }

        bool reactor::pure_poll_once() {
            for (auto c : _pollers) {
                if (c->pure_poll()) {
                    return true;
                }
            }
            return false;
        }

        namespace detail {

            class poller::registration_task final : public task {
            private:
                poller *_p;

            public:
                explicit registration_task(poller *p) : _p(p) {
                }
                virtual void run_and_dispose() noexcept override {
                    if (_p) {
                        engine().register_poller(_p->_pollfn.get());
                        _p->_registration_task = nullptr;
                    }
                    delete this;
                }
                task *waiting_task() noexcept override {
                    return nullptr;
                }
                void cancel() {
                    _p = nullptr;
                }
                void moved(poller *p) {
                    _p = p;
                }
            };

            class poller::deregistration_task final : public task {
            private:
                std::unique_ptr<pollfn> _p;

            public:
                explicit deregistration_task(std::unique_ptr<pollfn> &&p) : _p(std::move(p)) {
                }
                virtual void run_and_dispose() noexcept override {
                    engine().unregister_poller(_p.get());
                    delete this;
                }
                task *waiting_task() noexcept override {
                    return nullptr;
                }
            };

        }    // namespace detail

        void reactor::register_poller(pollfn *p) {
            _pollers.push_back(p);
        }

        void reactor::unregister_poller(pollfn *p) {
            _pollers.erase(std::find(_pollers.begin(), _pollers.end(), p));
        }

        void reactor::replace_poller(pollfn *old, pollfn *neww) {
            std::replace(_pollers.begin(), _pollers.end(), old, neww);
        }

        namespace detail {

            poller::poller(poller &&x) noexcept :
                _pollfn(std::move(x._pollfn)), _registration_task(std::exchange(x._registration_task, nullptr)) {
                if (_pollfn && _registration_task) {
                    _registration_task->moved(this);
                }
            }

            poller &poller::operator=(poller &&x) noexcept {
                if (this != &x) {
                    this->~poller();
                    new (this) poller(std::move(x));
                }
                return *this;
            }

            void poller::do_register() noexcept {
                // We can't just insert a poller into reactor::_pollers, because we
                // may be running inside a poller ourselves, and so in the middle of
                // iterating reactor::_pollers itself.  So we schedule a task to add
                // the poller instead.
                auto task = new registration_task(this);
                engine().add_task(task);
                _registration_task = task;
            }

            poller::~poller() {
                // We can't just remove the poller from reactor::_pollers, because we
                // may be running inside a poller ourselves, and so in the middle of
                // iterating reactor::_pollers itself.  So we schedule a task to remove
                // the poller instead.
                //
                // Since we don't want to call the poller after we exit the destructor,
                // we replace it atomically with another one, and schedule a task to
                // delete the replacement.
                if (_pollfn) {
                    if (_registration_task) {
                        // not added yet, so don't do it at all.
                        _registration_task->cancel();
                        delete _registration_task;
                    } else if (!engine()._finished_running_tasks) {
                        // If _finished_running_tasks, the call to add_task() below will just
                        // leak it, since no one will call task::run_and_dispose(). Just leave
                        // the poller there, the reactor will never use it.
                        auto dummy = make_pollfn([] { return false; });
                        auto dummy_p = dummy.get();
                        auto task = new deregistration_task(std::move(dummy));
                        engine().add_task(task);
                        engine().replace_poller(_pollfn.get(), dummy_p);
                    }
                }
            }

        }    // namespace detail

        syscall_work_queue::syscall_work_queue() : _pending(), _completed(), _start_eventfd(0) {
        }

        void syscall_work_queue::submit_item(std::unique_ptr<syscall_work_queue::work_item> item) {
            (void)_queue_has_room.wait().then_wrapped([this, item = std::move(item)](future<> f) mutable {
                // propagate wait failure via work_item
                if (f.failed()) {
                    item->set_exception(f.get_exception());
                    return;
                }
                _pending.push(item.release());
                _start_eventfd.signal(1);
            });
        }

        unsigned syscall_work_queue::complete() {
            std::array<work_item *, queue_length> tmp_buf;
            auto end = tmp_buf.data();
            auto nr = _completed.consume_all([&](work_item *wi) { *end++ = wi; });
            for (auto p = tmp_buf.data(); p != end; ++p) {
                auto wi = *p;
                wi->complete();
                delete wi;
            }
            _queue_has_room.signal(nr);
            return nr;
        }

        smp_message_queue::smp_message_queue(reactor *from, reactor *to) : _pending(to), _completed(from) {
        }

        smp_message_queue::~smp_message_queue() {
            if (_pending.remote != _completed.remote) {
                _tx.a.~aa();
            }
        }

        void smp_message_queue::stop() {
            _metrics.clear();
        }

        void smp_message_queue::move_pending() {
            auto begin = _tx.a.pending_fifo.cbegin();
            auto end = _tx.a.pending_fifo.cend();
            end = _pending.push(begin, end);
            if (begin == end) {
                return;
            }
            auto nr = end - begin;
            _pending.maybe_wakeup();
            _tx.a.pending_fifo.erase(begin, end);
            _current_queue_length += nr;
            _last_snt_batch = nr;
            _sent += nr;
        }

        bool smp_message_queue::pure_poll_tx() const {
            // can't use read_available(), not available on older boost
            // empty() is not const, so need const_cast.
            return !const_cast<lf_queue &>(_completed).empty();
        }

        void smp_message_queue::submit_item(shard_id t, smp_timeout_clock::time_point timeout,
                                            std::unique_ptr<smp_message_queue::work_item> item) {
            // matching signal() in process_completions()
            auto ssg_id = detail::smp_service_group_id(item->ssg);
            auto &sem = get_smp_service_groups_semaphore(ssg_id, t);
            // Future indirectly forwarded to `item`.
            (void)get_units(sem, 1, timeout)
                .then_wrapped(
                    [this, item = std::move(item)](future<smp_service_group_semaphore_units> units_fut) mutable {
                        if (units_fut.failed()) {
                            item->fail_with(units_fut.get_exception());
                            ++_compl;
                            ++_last_cmpl_batch;
                            return;
                        }
                        _tx.a.pending_fifo.push_back(item.get());
                        // no exceptions from this point
                        item.release();
                        units_fut.get0().release();
                        if (_tx.a.pending_fifo.size() >= batch_size) {
                            move_pending();
                        }
                    });
        }

        void smp_message_queue::respond(work_item *item) {
            _completed_fifo.push_back(item);
            if (_completed_fifo.size() >= batch_size || engine()._stopped) {
                flush_response_batch();
            }
        }

        void smp_message_queue::flush_response_batch() {
            if (!_completed_fifo.empty()) {
                auto begin = _completed_fifo.cbegin();
                auto end = _completed_fifo.cend();
                end = _completed.push(begin, end);
                if (begin == end) {
                    return;
                }
                _completed.maybe_wakeup();
                _completed_fifo.erase(begin, end);
            }
        }

        bool smp_message_queue::has_unflushed_responses() const {
            return !_completed_fifo.empty();
        }

        bool smp_message_queue::pure_poll_rx() const {
            // can't use read_available(), not available on older boost
            // empty() is not const, so need const_cast.
            return !const_cast<lf_queue &>(_pending).empty();
        }

        void smp_message_queue::lf_queue::maybe_wakeup() {
            // Called after lf_queue_base::push().
            //
            // This is read-after-write, which wants memory_order_seq_cst,
            // but we insert that barrier using systemwide_memory_barrier()
            // because seq_cst is so expensive.
            //
            // However, we do need a compiler barrier:
            std::atomic_signal_fence(std::memory_order_seq_cst);
            if (remote->_sleeping.load(std::memory_order_relaxed)) {
                // We are free to clear it, because we're sending a signal now
                remote->_sleeping.store(false, std::memory_order_relaxed);
                remote->wakeup();
            }
        }

        smp_message_queue::lf_queue::~lf_queue() {
            consume_all([](work_item *ptr) { delete ptr; });
        }

        template<size_t PrefetchCnt, typename Func>
        size_t smp_message_queue::process_queue(lf_queue &q, Func process) {
            // copy batch to local memory in order to minimize
            // time in which cross-cpu data is accessed
            work_item *items[queue_length + PrefetchCnt];
            work_item *wi;
            if (!q.pop(wi))
                return 0;
            // start prefetching first item before popping the rest to overlap memory
            // access with potential cache miss the second pop may cause
            prefetch<2>(wi);
            auto nr = q.pop(items);
            std::fill(std::begin(items) + nr, std::begin(items) + nr + PrefetchCnt, nr ? items[nr - 1] : wi);
            unsigned i = 0;
            do {
                prefetch_n<2>(std::begin(items) + i, std::begin(items) + i + PrefetchCnt);
                process(wi);
                wi = items[i++];
            } while (i <= nr);

            return nr + 1;
        }

        size_t smp_message_queue::process_completions(shard_id t) {
            auto nr = process_queue<prefetch_cnt * 2>(_completed, [t](work_item *wi) {
                wi->complete();
                auto ssg_id = smp_service_group_id(wi->ssg);
                get_smp_service_groups_semaphore(ssg_id, t).signal();
                delete wi;
            });
            _current_queue_length -= nr;
            _compl += nr;
            _last_cmpl_batch = nr;

            return nr;
        }

        void smp_message_queue::flush_request_batch() {
            if (!_tx.a.pending_fifo.empty()) {
                move_pending();
            }
        }

        size_t smp_message_queue::process_incoming() {
            auto nr = process_queue<prefetch_cnt>(_pending, [](work_item *wi) { wi->process(); });
            _received += nr;
            _last_rcv_batch = nr;
            return nr;
        }

        void smp_message_queue::start(unsigned cpuid) {
            _tx.init();
            namespace sm = nil::actor::metrics;
            char instance[10];
            std::snprintf(instance, sizeof(instance), "%u-%u", this_shard_id(), cpuid);
            _metrics.add_group(
                "smp",
                {// queue_length     value:GAUGE:0:U
                 // Absolute value of num packets in last tx batch.
                 sm::make_queue_length("send_batch_queue_length", _last_snt_batch,
                                       sm::description("Current send batch queue length"),
                                       {sm::shard_label(instance)})(sm::metric_disabled),
                 sm::make_queue_length("receive_batch_queue_length", _last_rcv_batch,
                                       sm::description("Current receive batch queue length"),
                                       {sm::shard_label(instance)})(sm::metric_disabled),
                 sm::make_queue_length("complete_batch_queue_length", _last_cmpl_batch,
                                       sm::description("Current complete batch queue length"),
                                       {sm::shard_label(instance)})(sm::metric_disabled),
                 sm::make_queue_length("send_queue_length", _current_queue_length,
                                       sm::description("Current send queue length"),
                                       {sm::shard_label(instance)})(sm::metric_disabled),
                 // total_operations value:DERIVE:0:U
                 sm::make_derive("total_received_messages", _received,
                                 sm::description("Total number of received messages"),
                                 {sm::shard_label(instance)})(sm::metric_disabled),
                 // total_operations value:DERIVE:0:U
                 sm::make_derive("total_sent_messages", _sent, sm::description("Total number of sent messages"),
                                 {sm::shard_label(instance)})(sm::metric_disabled),
                 // total_operations value:DERIVE:0:U
                 sm::make_derive("total_completed_messages", _compl,
                                 sm::description("Total number of messages completed"),
                                 {sm::shard_label(instance)})(sm::metric_disabled)});
        }

        readable_eventfd writeable_eventfd::read_side() {
            return readable_eventfd(_fd.dup());
        }

        file_desc writeable_eventfd::try_create_eventfd(size_t initial) {
            assert(size_t(int(initial)) == initial);
            return file_desc::eventfd(initial, EFD_CLOEXEC);
        }

        void writeable_eventfd::signal(size_t count) {
            uint64_t c = count;
#if BOOST_OS_LINUX
            auto r = _fd.write(&c, sizeof(c));
#else
            auto r = epoll_shim_write(_fd.get(), &c, sizeof(c));
#endif
            assert(r == sizeof(c));
        }

        writeable_eventfd readable_eventfd::write_side() {
            return writeable_eventfd(_fd.get_file_desc().dup());
        }

        file_desc readable_eventfd::try_create_eventfd(size_t initial) {
            assert(size_t(int(initial)) == initial);
            return file_desc::eventfd(initial, EFD_CLOEXEC | EFD_NONBLOCK);
        }

        future<size_t> readable_eventfd::wait() {
            return engine().readable(*_fd._s).then([this] {
                uint64_t count;
#if BOOST_OS_LINUX
                int r = ::read(_fd.get_fd(), &count, sizeof(count));
#else
                int r = epoll_shim_read(_fd.get_fd(), &count, sizeof(count));
#endif
                assert(r == sizeof(count));
                return make_ready_future<size_t>(count);
            });
        }

        void schedule(task *t) noexcept {
            engine().add_task(t);
        }

        void schedule_urgent(task *t) noexcept {
            engine().add_urgent_task(t);
        }

    }    // namespace actor
}    // namespace nil

bool operator==(const ::sockaddr_in a, const ::sockaddr_in b) {
    return (a.sin_addr.s_addr == b.sin_addr.s_addr) && (a.sin_port == b.sin_port);
}

namespace nil {
    namespace actor {

        void register_network_stack(
            const sstring &name, const boost::program_options::options_description &opts,
            noncopyable_function<future<std::unique_ptr<network_stack>>(boost::program_options::variables_map)> create,
            bool make_default) {
            return network_stack_registry::register_stack(std::move(name), opts, std::move(create), make_default);
        }

        static bool kernel_supports_aio_fsync() {
            return kernel_uname().whitelisted({"4.18"});
        }

        boost::program_options::options_description reactor::get_options_description(reactor_config cfg) {
            boost::program_options::options_description opts("Core options");
            auto net_stack_names = network_stack_registry::list();
            opts.add_options()("network-stack", boost::program_options::value<std::string>(),
                               format("select network stack (valid values: {})",
                                      format_separated(net_stack_names.begin(), net_stack_names.end(), ", "))
                                   .c_str())("poll-mode", "poll continuously (100% cpu use)")(
                "idle-poll-time-us", boost::program_options::value<unsigned>()->default_value(calculate_poll_time() / 1us),
                "idle polling time in microseconds (reduce for overprovisioned environments or laptops)")(
                "poll-aio", boost::program_options::value<bool>()->default_value(true),
                "busy-poll for disk I/O (reduces latency and increases throughput)")(
                "task-quota-ms", boost::program_options::value<double>()->default_value(cfg.task_quota / 1ms),
                "Max time (ms) between polls")("max-task-backlog", boost::program_options::value<unsigned>()->default_value(1000),
                                               "Maximum number of task backlog to allow; above this we ignore I/O")(
                "blocked-reactor-notify-ms", boost::program_options::value<unsigned>()->default_value(20000),
                "threshold in miliseconds over which the reactor is considered blocked if no progress is made")(
                "blocked-reactor-reports-per-minute", boost::program_options::value<unsigned>()->default_value(5),
                "Maximum number of backtraces reported by stall detector per minute")(
                "relaxed-dma", "allow using buffered I/O if DMA is not available (reduces performance)")(
                "linux-aio-nowait",
                boost::program_options::value<bool>()->default_value(aio_nowait_supported),
                "use the Linux NOWAIT AIO feature, which reduces reactor stalls due to aio (autodetected)")(
                "unsafe-bypass-fsync", boost::program_options::value<bool>()->default_value(false),
                "Bypass fsync(), may result in data loss. Use for testing on consumer drives")(
                "overprovisioned",
                "run in an overprovisioned environment (such as docker or a laptop); equivalent to --idle-poll-time-us "
                "0 "
                "--thread-affinity 0 --poll-aio 0")("abort-on-actor-bad-alloc",
                                                    "abort when actor allocator cannot allocate memory")(
                "force-aio-syscalls", boost::program_options::value<bool>()->default_value(false),
                "Force io_getevents(2) to issue a system call, instead of bypassing the kernel when possible."
                " This makes strace output more useful, but slows down the application")(
                "dump-memory-diagnostics-on-alloc-failure-kind", boost::program_options::value<std::string>()->default_value("critical"),
                "Dump diagnostics of the actor allocator state on allocation failure."
                " Accepted values: never, critical (default), always. When set to critical, only allocations marked as "
                "critical will trigger diagnostics dump."
                " The diagnostics will be written to the actor_memory logger, with error level."
                " Note that if the actor_memory logger is set to debug or trace level, the diagnostics will be "
                "logged "
                "irrespective of this setting.")
                ("shard0-mem-scale", boost::program_options::value<std::size_t>()->default_value(1),
                 "The ratio of how much the zero shard memory is larger than the rest")
                ("reactor-backend",
                boost::program_options::value<reactor_backend_selector>()->default_value(reactor_backend_selector::default_backend()),
                format("Internal reactor implementation ({})", reactor_backend_selector::available()).c_str())(
                "aio-fsync", boost::program_options::value<bool>()->default_value(kernel_supports_aio_fsync()),
                "Use Linux aio for fsync() calls. This reduces latency; requires Linux 4.18 or later.")
#ifdef ACTOR_HEAPPROF
                ("heapprof", "enable actor heap profiling")
#endif
                ;
            if (cfg.auto_handle_sigint_sigterm) {
                opts.add_options()("no-handle-interrupt", "ignore SIGINT (for gdb)");
            }
            opts.add(network_stack_registry::options_description());
            return opts;
        }

        boost::program_options::options_description smp::get_options_description() {
            namespace bpo = boost::program_options;
            bpo::options_description opts("SMP options");
            opts.add_options()("smp,c", bpo::value<unsigned>(), "number of threads (default: one per CPU)")(
                "cpuset", bpo::value<cpuset_bpo_wrapper>(), "CPUs to use (in cpuset(7) format; default: all))")(
                "memory,m", bpo::value<std::string>(), "memory to use, in bytes (ex: 4G) (default: all)")(
                "reserve-memory", bpo::value<std::string>(), "memory reserved to OS (if --memory not specified)")(
                "hugepages", bpo::value<std::string>(),
                "path to accessible hugetlbfs mount (typically /dev/hugepages/something)")(
                "lock-memory", bpo::value<bool>(),
                "lock all memory (prevents swapping)")("thread-affinity", bpo::value<bool>()->default_value(true),
                                                       "pin threads to their cpus (disable for overprovisioning)")
#ifdef ACTOR_HAVE_HWLOC
                ("num-io-queues", bpo::value<unsigned>(),
                 "Number of IO queues. Each IO unit will be responsible for a fraction of the IO requests. Defaults to "
                 "the "
                 "number of threads")("num-io-groups", bpo::value<unsigned>(),
                                      "Number of IO groups. Each IO group will be responsible for a fraction of the IO "
                                      "requests. Defaults to the number of NUMA nodes")(
                    "max-io-requests", bpo::value<unsigned>(),
                    "Maximum amount of concurrent requests to be sent to the disk. Defaults to 128 times the number of "
                    "IO "
                    "queues")
#else
                ("max-io-requests", bpo::value<unsigned>(),
                 "Maximum amount of concurrent requests to be sent to the disk. Defaults to 128 times the number of "
                 "processors")
#endif
                    ("io-properties-file", bpo::value<std::string>(),
                     "path to a YAML file describing the characteristics of the I/O Subsystem")(
                        "io-properties", bpo::value<std::string>(),
                        "a YAML string describing the characteristics of the I/O Subsystem")(
                        "mbind", bpo::value<bool>()->default_value(true), "enable mbind")
#ifndef ACTOR_NO_EXCEPTION_HACK
                        ("enable-glibc-exception-scaling-workaround", bpo::value<bool>()->default_value(true),
                         "enable workaround for glibc/gcc c++ exception scalablity problem")
#endif
#ifdef ACTOR_HAVE_HWLOC
                            ("allow-cpus-in-remote-numa-nodes", bpo::value<bool>()->default_value(true),
                             "if some CPUs are found not to have any local NUMA nodes, allow assigning them to remote "
                             "ones")
#endif
                ;
            return opts;
        }

        thread_local scollectd::impl scollectd_impl;

        scollectd::impl &scollectd::get_impl() {
            return scollectd_impl;
        }

        struct reactor_deleter {
            void operator()(reactor *p) {
                p->~reactor();
                free(p);
            }
        };

        thread_local std::unique_ptr<reactor, reactor_deleter> reactor_holder;

        std::vector<posix_thread> smp::_threads;
        std::vector<std::function<void()>> smp::_thread_loops;
        boost::optional<boost::barrier> smp::_all_event_loops_done;
        std::vector<reactor *> smp::_reactors;
        std::unique_ptr<smp_message_queue *[], smp::qs_deleter> smp::_qs;
        std::thread::id smp::_tmain;
        unsigned smp::count = 1;
        bool smp::_using_dpdk;

        void smp::start_all_queues() {
            for (unsigned c = 0; c < count; c++) {
                if (c != this_shard_id()) {
                    _qs[c][this_shard_id()].start(c);
                }
            }
            alien::smp::_qs[this_shard_id()].start();
        }

#ifdef ACTOR_HAVE_DPDK

        int dpdk_thread_adaptor(void *f) {
            (*static_cast<std::function<void()> *>(f))();
            return 0;
        }

#endif

        void smp::join_all() {
#ifdef ACTOR_HAVE_DPDK
            if (_using_dpdk) {
                rte_eal_mp_wait_lcore();
                return;
            }
#endif
            for (auto &&t : smp::_threads) {
                t.join();
            }
        }

        void smp::pin(unsigned cpu_id) {
            if (_using_dpdk) {
                // dpdk does its own pinning
                return;
            }
            pin_this_thread(cpu_id);
        }

        void smp::arrive_at_event_loop_end() {
            if (_all_event_loops_done) {
                _all_event_loops_done->wait();
            }
        }

        void smp::allocate_reactor(unsigned id, reactor_backend_selector rbs, reactor_config cfg) {
            assert(!reactor_holder);

            // we cannot just write "local_engine = new reactor" since reactor's constructor
            // uses local_engine
            void *buf;
            int r = posix_memalign(&buf, cache_line_size, sizeof(reactor));
            assert(r == 0);
            local_engine = reinterpret_cast<reactor *>(buf);
            *detail::this_shard_id_ptr() = id;
            new (buf) reactor(id, std::move(rbs), cfg);
            reactor_holder.reset(local_engine);
        }

        void smp::cleanup() {
            smp::_threads = std::vector<posix_thread>();
            _thread_loops.clear();
        }

        void smp::cleanup_cpu() {
            size_t cpuid = this_shard_id();

            if (_qs) {
                for (unsigned i = 0; i < smp::count; i++) {
                    _qs[i][cpuid].stop();
                }
            }
            if (alien::smp::_qs) {
                alien::smp::_qs[cpuid].stop();
            }
        }

        void smp::create_thread(std::function<void()> thread_loop) {
            if (_using_dpdk) {
                _thread_loops.push_back(std::move(thread_loop));
            } else {
                _threads.emplace_back(std::move(thread_loop));
            }
        }

        // Installs handler for Signal which ensures that Func is invoked only once
        // in the whole program and that after it is invoked the default handler is restored.
        template<int Signal, void (*Func)()>
        void install_oneshot_signal_handler() {
            static bool handled = false;
            static util::spinlock lock;

            struct sigaction sa;
            sa.sa_sigaction = [](int sig, siginfo_t *info, void *p) {
                std::lock_guard<util::spinlock> g(lock);
                if (!handled) {
                    handled = true;
                    Func();
                    signal(sig, SIG_DFL);
                }
            };
            sigfillset(&sa.sa_mask);
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            if (Signal == SIGSEGV) {
                sa.sa_flags |= SA_ONSTACK;
            }
            auto r = ::sigaction(Signal, &sa, nullptr);
            throw_system_error_on(r == -1);
        }

        static void sigsegv_action() noexcept {
            print_with_backtrace("Segmentation fault");
        }

        static void sigabrt_action() noexcept {
            print_with_backtrace("Aborting");
        }

        void smp::qs_deleter::operator()(smp_message_queue **qs) const {
            for (unsigned i = 0; i < smp::count; i++) {
                for (unsigned j = 0; j < smp::count; j++) {
                    qs[i][j].~smp_message_queue();
                }
                ::operator delete[](qs[i]);
            }
            delete[](qs);
        }

        class disk_config_params {
        private:
            unsigned _num_io_groups = 0;
            boost::optional<unsigned> _capacity;
            std::unordered_map<dev_t, mountpoint_params> _mountpoints;
            std::chrono::duration<double> _latency_goal;

        public:
            uint64_t per_io_group(uint64_t qty, unsigned nr_groups) const noexcept {
                return std::max(qty / nr_groups, uint64_t(1));
            }

            unsigned num_io_groups() const noexcept {
                return _num_io_groups;
            }

            std::chrono::duration<double> latency_goal() const {
                return _latency_goal;
            }

            void parse_config(boost::program_options::variables_map &configuration) {
                actor_logger.debug("smp::count: {}", smp::count);
                _latency_goal = std::chrono::duration_cast<std::chrono::duration<double>>(
                    configuration["task-quota-ms"].as<double>() * 1.5 * 1ms);
                actor_logger.debug("latency_goal: {}", latency_goal().count());

                if (configuration.count("max-io-requests")) {
                    _capacity = configuration["max-io-requests"].as<unsigned>();
                }

                if (configuration.count("num-io-groups")) {
                    _num_io_groups = configuration["num-io-groups"].as<unsigned>();
                    if (!_num_io_groups) {
                        throw std::runtime_error("num-io-groups must be greater than zero");
                    }
                } else if (configuration.count("num-io-queues")) {
                    actor_logger.warn("the --num-io-queues option is deprecated, switch to --num-io-groups instead");
                    _num_io_groups = configuration["num-io-queues"].as<unsigned>();
                    if (!_num_io_groups) {
                        throw std::runtime_error("num-io-queues must be greater than zero");
                    }
                }
                if (configuration.count("io-properties-file") && configuration.count("io-properties")) {
                    throw std::runtime_error(
                        "Both io-properties and io-properties-file specified. Don't know which to trust!");
                }

                boost::optional<YAML::Node> doc;
                if (configuration.count("io-properties-file")) {
                    doc = YAML::LoadFile(configuration["io-properties-file"].as<std::string>());
                } else if (configuration.count("io-properties")) {
                    doc = YAML::Load(configuration["io-properties"].as<std::string>());
                }

                if (doc) {
                    for (auto &&section : *doc) {
                        auto sec_name = section.first.as<std::string>();
                        if (sec_name != "disks") {
                            throw std::runtime_error(
                                fmt::format("While parsing I/O options: section {} currently unsupported.", sec_name));
                        }
                        auto disks = section.second.as<std::vector<mountpoint_params>>();
                        for (auto &d : disks) {
                            struct ::stat buf;
                            auto ret = stat(d.mountpoint.c_str(), &buf);
                            if (ret < 0) {
                                throw std::runtime_error(fmt::format("Couldn't stat {}", d.mountpoint));
                            }
                            if (_mountpoints.count(buf.st_dev)) {
                                throw std::runtime_error(fmt::format("Mountpoint {} already configured", d.mountpoint));
                            }
                            if (_mountpoints.size() >= reactor::max_queues) {
                                throw std::runtime_error(
                                    fmt::format("Configured number of queues {} is larger than the maximum {}",
                                                _mountpoints.size(), reactor::max_queues));
                            }
                            if (d.read_bytes_rate == 0 || d.write_bytes_rate == 0 || d.read_req_rate == 0 ||
                                d.write_req_rate == 0) {
                                throw std::runtime_error(fmt::format("R/W bytes and req rates must not be zero"));
                            }

                            actor_logger.debug("dev_id: {} mountpoint: {}", buf.st_dev, d.mountpoint);
                            _mountpoints.emplace(buf.st_dev, d);
                        }
                    }
                }

                // Placeholder for unconfigured disks.
                mountpoint_params d = {};
                _mountpoints.emplace(0, d);
            }

            struct io_group::config generate_group_config(dev_t devid, unsigned nr_groups) const noexcept {
                actor_logger.debug("generate_group_config dev_id: {}", devid);
                const mountpoint_params &p = _mountpoints.at(devid);
                struct io_group::config cfg;
                uint64_t max_bandwidth = std::max(p.read_bytes_rate, p.write_bytes_rate);
                uint64_t max_iops = std::max(p.read_req_rate, p.write_req_rate);

                if (!_capacity) {
                    if (max_bandwidth != std::numeric_limits<uint64_t>::max()) {
                        cfg.max_bytes_count = io_queue::read_request_base_count *
                                              per_io_group(max_bandwidth * latency_goal().count(), nr_groups);
                    }
                    if (max_iops != std::numeric_limits<uint64_t>::max()) {
                        cfg.max_req_count = io_queue::read_request_base_count *
                                            per_io_group(max_iops * latency_goal().count(), nr_groups);
                    }
                } else {
                    // Legacy configuration when only concurrency is specified.
                    cfg.max_req_count =
                        io_queue::read_request_base_count * std::min(*_capacity, reactor::max_aio_per_queue);
                    // specify size in terms of 16kB IOPS.
                    cfg.max_bytes_count = io_queue::read_request_base_count * (cfg.max_req_count << 14);
                }
                return cfg;
            }

            struct io_queue::config generate_config(dev_t devid) const {
                actor_logger.debug("generate_config dev_id: {}", devid);
                const mountpoint_params &p = _mountpoints.at(devid);
                struct io_queue::config cfg;
                uint64_t max_bandwidth = std::max(p.read_bytes_rate, p.write_bytes_rate);
                uint64_t max_iops = std::max(p.read_req_rate, p.write_req_rate);

                cfg.devid = devid;
                cfg.disk_bytes_write_to_read_multiplier = io_queue::read_request_base_count;
                cfg.disk_req_write_to_read_multiplier = io_queue::read_request_base_count;

                if (!_capacity) {
                    if (max_bandwidth != std::numeric_limits<uint64_t>::max()) {
                        cfg.disk_bytes_write_to_read_multiplier =
                            (io_queue::read_request_base_count * p.read_bytes_rate) / p.write_bytes_rate;
                        cfg.disk_us_per_byte = 1000000. / max_bandwidth;
                    }
                    if (max_iops != std::numeric_limits<uint64_t>::max()) {
                        cfg.disk_req_write_to_read_multiplier =
                            (io_queue::read_request_base_count * p.read_req_rate) / p.write_req_rate;
                        cfg.disk_us_per_request = 1000000. / max_iops;
                    }
                    cfg.mountpoint = p.mountpoint;
                } else {
                    // For backwards compatibility
                    cfg.capacity = *_capacity;
                }
                return cfg;
            }

            auto device_ids() {
                return boost::adaptors::keys(_mountpoints);
            }
        };

        void smp::register_network_stacks() {
            register_posix_stack();
#if BOOST_OS_LINUX
            register_native_stack();
#endif
        }

        void smp::configure(boost::program_options::variables_map configuration, reactor_config reactor_cfg) {
#if !defined(ACTOR_NO_EXCEPTION_HACK) && BOOST_OS_LINUX || BOOST_OS_BSD_FREE > BOOST_VERSION_NUMBER(9, 1, 0)
            if (configuration["enable-glibc-exception-scaling-workaround"].as<bool>()) {
                init_phdr_cache();
            }
#endif

            // Mask most, to prevent threads (esp. dpdk helper threads)
            // from servicing a signal.  Individual reactors will unmask signals
            // as they become prepared to handle them.
            //
            // We leave some signals unmasked since we don't handle them ourself.
            sigset_t sigs;
            sigfillset(&sigs);
            for (auto sig : {SIGHUP, SIGQUIT, SIGILL, SIGABRT, SIGFPE, SIGSEGV, SIGALRM, SIGCONT, SIGSTOP, SIGTSTP,
                             SIGTTIN, SIGTTOU}) {
                sigdelset(&sigs, sig);
            }
            if (!reactor_cfg.auto_handle_sigint_sigterm) {
                sigdelset(&sigs, SIGINT);
                sigdelset(&sigs, SIGTERM);
            }
            pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

#ifndef ACTOR_ASAN_ENABLED
            // We don't need to handle SIGSEGV when asan is enabled.
            install_oneshot_signal_handler<SIGSEGV, sigsegv_action>();
#else
            (void)sigsegv_action;
#endif
            install_oneshot_signal_handler<SIGABRT, sigabrt_action>();

#ifdef ACTOR_HAVE_DPDK
            _using_dpdk = configuration.count("dpdk-pmd");
#endif
            auto thread_affinity = configuration["thread-affinity"].as<bool>();
            if (configuration.count("overprovisioned") && configuration["thread-affinity"].defaulted()) {
                thread_affinity = false;
            }
            if (!thread_affinity && _using_dpdk) {
                fmt::print("warning: --thread-affinity 0 ignored in dpdk mode\n");
            }
            auto mbind = configuration["mbind"].as<bool>();
            if (!thread_affinity) {
                mbind = false;
            }

            smp::count = 1;
            smp::_tmain = std::this_thread::get_id();
            auto nr_cpus = resource::nr_processing_units();
            resource::cpuset cpu_set;
            auto cgroup_cpu_set = cgroup::cpu_set();

            std::copy(boost::counting_iterator<unsigned>(0), boost::counting_iterator<unsigned>(nr_cpus),
                      std::inserter(cpu_set, cpu_set.end()));

            if (configuration.count("cpuset")) {
                cpu_set = configuration["cpuset"].as<cpuset_bpo_wrapper>().value;
                if (cgroup_cpu_set && *cgroup_cpu_set != cpu_set) {
                    // CPUs that are not available are those pinned by
                    // --cpuset but not by cgroups, if mounted.
                    std::set<unsigned int> not_available_cpus;
                    std::set_difference(cpu_set.begin(), cpu_set.end(), cgroup_cpu_set->begin(), cgroup_cpu_set->end(),
                                        std::inserter(not_available_cpus, not_available_cpus.end()));

                    if (!not_available_cpus.empty()) {
                        std::ostringstream not_available_cpus_list;
                        for (auto cpu_id : not_available_cpus) {
                            not_available_cpus_list << " " << cpu_id;
                        }
                        actor_logger.error("Bad value for --cpuset:{} not allowed. Shutting down.",
                                             not_available_cpus_list.str());
                        exit(1);
                    }
                }
            } else if (cgroup_cpu_set) {
                cpu_set = *cgroup_cpu_set;
            }

            if (configuration.count("smp")) {
                nr_cpus = configuration["smp"].as<unsigned>();
            } else {
                nr_cpus = cpu_set.size();
            }
            smp::count = nr_cpus;
            _reactors.resize(nr_cpus);
            resource::configuration rc;
            if (configuration.count("memory")) {
                rc.total_memory = parse_memory_size(configuration["memory"].as<std::string>());
#ifdef ACTOR_HAVE_DPDK
                if (configuration.count("hugepages") &&
                    !configuration["network-stack"].as<std::string>().compare("native") && _using_dpdk) {
                    size_t dpdk_memory = dpdk::eal::mem_size(smp::count);

                    if (dpdk_memory >= rc.total_memory) {
                        std::cerr << "Can't run with the given amount of memory: ";
                        std::cerr << configuration["memory"].as<std::string>();
                        std::cerr << ". Consider giving more." << std::endl;
                        exit(1);
                    }

                    //
                    // Subtract the memory we are about to give to DPDK from the total
                    // amount of memory we are allowed to use.
                    //
                    rc.total_memory.value() -= dpdk_memory;
                }
#endif
            }
            if (configuration.count("reserve-memory")) {
                rc.reserve_memory = parse_memory_size(configuration["reserve-memory"].as<std::string>());
            }
            boost::optional<std::string> hugepages_path;
            if (configuration.count("hugepages")) {
                hugepages_path = configuration["hugepages"].as<std::string>();
            }
            auto mlock = false;
            if (configuration.count("lock-memory")) {
                mlock = configuration["lock-memory"].as<bool>();
            }
            if (mlock) {
                auto extra_flags = 0;
#ifdef MCL_ONFAULT
                // Linux will serialize faulting in anonymous memory, and also
                // serialize marking them as locked. This can take many minutes on
                // terabyte class machines, so fault them in the future to spread
                // out the cost. This isn't good since we'll see contention if
                // multiple shards fault in memory at once, but if that work can be
                // in parallel to regular reactor work on other shards.
                extra_flags |= MCL_ONFAULT;    // Linux 4.4+
#endif
                auto r = mlockall(MCL_CURRENT | MCL_FUTURE | extra_flags);
                if (r) {
                    // Don't hard fail for now, it's hard to get the configuration right
                    fmt::print("warning: failed to mlockall: {}\n", strerror(errno));
                }
            }

            rc.cpus = smp::count;
            rc.cpu_set = std::move(cpu_set);

            disk_config_params disk_config;
            disk_config.parse_config(configuration);
            for (auto &id : disk_config.device_ids()) {
                rc.devices.push_back(id);
            }
            rc.num_io_groups = disk_config.num_io_groups();

#ifdef ACTOR_HAVE_HWLOC
            if (configuration["allow-cpus-in-remote-numa-nodes"].as<bool>()) {
                rc.assign_orphan_cpus = true;
            }
#endif
            rc.shard0scale = 48 * 16; // configuration["shard0-mem-scale"].as<size_t>();

            auto resources = resource::allocate(rc);
            std::vector<resource::cpu> allocations = std::move(resources.cpus);
            if (thread_affinity) {
                smp::pin(allocations[0].cpu_id);
            }
            memory::configure(allocations[0].mem, mbind, hugepages_path);

            if (configuration.count("abort-on-actor-bad-alloc")) {
                memory::enable_abort_on_allocation_failure();
            }

            if (configuration.count("dump-memory-diagnostics-on-alloc-failure-kind")) {
                memory::set_dump_memory_diagnostics_on_alloc_failure_kind(
                    configuration["dump-memory-diagnostics-on-alloc-failure-kind"].as<std::string>());
            }

            bool heapprof_enabled = configuration.count("heapprof");
            if (heapprof_enabled) {
                memory::set_heap_profiling_enabled(heapprof_enabled);
            }

#ifdef ACTOR_HAVE_DPDK
            if (smp::_using_dpdk) {
                dpdk::eal::cpuset cpus;
                for (auto &&a : allocations) {
                    cpus[a.cpu_id] = true;
                }
                dpdk::eal::init(cpus, configuration);
            }
#endif

            // Better to put it into the smp class, but at smp construction time
            // correct smp::count is not known.
            static boost::barrier reactors_registered(smp::count);
            static boost::barrier smp_queues_constructed(smp::count);
            static boost::barrier inited(smp::count);

            auto ioq_topology = std::move(resources.ioq_topology);

            std::unordered_map<dev_t, resource::device_io_topology> devices_topology;

            for (auto &id : disk_config.device_ids()) {
                auto io_info = ioq_topology.at(id);
                devices_topology.emplace(id, io_info);
            }

            auto alloc_io_queue = [&ioq_topology, &devices_topology, &disk_config](unsigned shard, dev_t id) {
                auto io_info = ioq_topology.at(id);
                auto group_idx = io_info.shard_to_group[shard];
                resource::device_io_topology &topology = devices_topology[id];
                std::shared_ptr<io_group> group;

                {
                    std::lock_guard _(topology.lock);
                    resource::device_io_topology::group &iog = topology.groups[group_idx];
                    if (iog.attached == 0) {
                        struct io_group::config gcfg = disk_config.generate_group_config(id, topology.groups.size());
                        iog.g = std::make_shared<io_group>(std::move(gcfg));
                        actor_logger.debug("allocate {} IO group", group_idx);
                    }
                    iog.attached++;
                    group = iog.g;
                }

                struct io_queue::config cfg = disk_config.generate_config(id);
                topology.queues[shard] = new io_queue(std::move(group), engine()._io_sink, std::move(cfg));
                actor_logger.debug("attached {} queue to {} IO group", shard, group_idx);
            };

            auto assign_io_queue = [&devices_topology](shard_id shard_id, dev_t dev_id) {
                io_queue *queue = devices_topology[dev_id].queues[shard_id];
                engine()._io_queues.emplace(dev_id, queue);
            };

            _all_event_loops_done.emplace(smp::count);

            auto backend_selector = configuration["reactor-backend"].as<reactor_backend_selector>();

            unsigned i;
            for (i = 1; i < smp::count; i++) {
                auto allocation = allocations[i];
                create_thread([configuration, &disk_config, hugepages_path, i, allocation, assign_io_queue,
                               alloc_io_queue, thread_affinity, heapprof_enabled, mbind, backend_selector,
                               reactor_cfg] {
                    try {
                        auto thread_name = nil::actor::format("reactor-{}", i);
                        detail::set_thread_name(pthread_setname_np, thread_name.c_str());
                        if (thread_affinity) {
                            smp::pin(allocation.cpu_id);
                        }
                        memory::configure(allocation.mem, mbind, hugepages_path);
                        if (heapprof_enabled) {
                            memory::set_heap_profiling_enabled(heapprof_enabled);
                        }
                        sigset_t mask;
                        sigfillset(&mask);
                        for (auto sig : {SIGSEGV}) {
                            sigdelset(&mask, sig);
                        }
                        auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
                        throw_pthread_error(r);
                        init_default_smp_service_group(i);
                        allocate_reactor(i, backend_selector, reactor_cfg);
                        _reactors[i] = &engine();
                        for (auto &dev_id : disk_config.device_ids()) {
                            alloc_io_queue(i, dev_id);
                        }
                        reactors_registered.wait();
                        smp_queues_constructed.wait();
                        start_all_queues();
                        for (auto &dev_id : disk_config.device_ids()) {
                            assign_io_queue(i, dev_id);
                        }
                        inited.wait();
                        engine().configure(configuration);
                        engine().run();
                    } catch (const std::exception &e) {
                        actor_logger.error(e.what());
                        _exit(1);
                    }
                });
            }

            init_default_smp_service_group(0);
            try {
                allocate_reactor(0, backend_selector, reactor_cfg);
            } catch (const std::exception &e) {
                actor_logger.error(e.what());
                _exit(1);
            }

            _reactors[0] = &engine();
            for (auto &dev_id : disk_config.device_ids()) {
                alloc_io_queue(0, dev_id);
            }

#ifdef ACTOR_HAVE_DPDK
            if (_using_dpdk) {
                auto it = _thread_loops.begin();
                RTE_LCORE_FOREACH_SLAVE(i) {
                    rte_eal_remote_launch(dpdk_thread_adaptor, static_cast<void *>(&*(it++)), i);
                }
            }
#endif

            reactors_registered.wait();
            smp::_qs = decltype(smp::_qs) {new smp_message_queue *[smp::count], qs_deleter {}};
            for (unsigned k = 0; k < smp::count; k++) {
                smp::_qs[k] =
                    reinterpret_cast<smp_message_queue *>(operator new[](sizeof(smp_message_queue) * smp::count));
                for (unsigned j = 0; j < smp::count; ++j) {
                    new (&smp::_qs[k][j]) smp_message_queue(_reactors[j], _reactors[k]);
                }
            }
            alien::smp::_qs = alien::smp::create_qs(_reactors);
            smp_queues_constructed.wait();
            start_all_queues();
            for (auto &dev_id : disk_config.device_ids()) {
                assign_io_queue(0, dev_id);
            }
            inited.wait();

            engine().configure(configuration);
            // The raw `new` is necessary because of the private constructor of `lowres_clock_impl`.
            engine()._lowres_clock_impl = std::unique_ptr<lowres_clock_impl>(new lowres_clock_impl);
        }

        bool smp::poll_queues() {
            size_t got = 0;
            for (unsigned i = 0; i < count; i++) {
                if (this_shard_id() != i) {
                    auto &rxq = _qs[this_shard_id()][i];
                    rxq.flush_response_batch();
                    got += rxq.has_unflushed_responses();
                    got += rxq.process_incoming();
                    auto &txq = _qs[i][this_shard_id()];
                    txq.flush_request_batch();
                    got += txq.process_completions(i);
                }
            }
            return got != 0;
        }

        bool smp::pure_poll_queues() {
            for (unsigned i = 0; i < count; i++) {
                if (this_shard_id() != i) {
                    auto &rxq = _qs[this_shard_id()][i];
                    rxq.flush_response_batch();
                    auto &txq = _qs[i][this_shard_id()];
                    txq.flush_request_batch();
                    if (rxq.pure_poll_rx() || txq.pure_poll_tx() || rxq.has_unflushed_responses()) {
                        return true;
                    }
                }
            }
            return false;
        }

        detail::preemption_monitor bootstrap_preemption_monitor {};
        __thread const detail::preemption_monitor *g_need_preempt = &bootstrap_preemption_monitor;

        __thread reactor *local_engine;

        void report_exception(std::string_view message, std::exception_ptr eptr) noexcept {
            actor_logger.error("{}: {}", message, eptr);
        }

        future<> check_direct_io_support(std::string_view path) noexcept {
            struct w {
                sstring path;
                open_flags flags;
                std::function<future<>()> cleanup;

                static w parse(const actor::sstring &path, boost::optional<directory_entry_type> type) {
                    if (!type) {
                        throw std::invalid_argument(format("Could not open file at {}. Make sure it exists", path));
                    }

                    if (type == directory_entry_type::directory) {
                        auto fpath = path + "/.o_direct_test";
                        return w {fpath, open_flags::wo | open_flags::create | open_flags::truncate,
                                  [fpath] { return remove_file(fpath); }};
                    } else if ((type == directory_entry_type::regular) || (type == directory_entry_type::link)) {
                        return w {path, open_flags::ro, [] { return make_ready_future<>(); }};
                    } else {
                        throw std::invalid_argument(
                            format("{} neither a directory nor file. Can't be opened with O_DIRECT", path));
                    }
                };
            };

            // Allocating memory for a sstring can throw, hence the futurize_invoke
            return futurize_invoke([path] {
                return engine().file_type(path).then([path = sstring(path)](auto type) {
                    auto w = w::parse(path, type);
                    return open_file_dma(w.path, w.flags)
                        .then_wrapped([path = w.path, cleanup = std::move(w.cleanup)](future<file> f) {
                            try {
                                auto fd = f.get0();
                                return cleanup().finally([fd = std::move(fd)]() mutable { return fd.close(); });
                            } catch (std::system_error &e) {
                                if (e.code() == std::error_code(EINVAL, std::system_category())) {
                                    report_exception(
                                        format("Could not open file at {}. Does your filesystem support O_DIRECT?",
                                               path),
                                        std::current_exception());
                                }
                                throw;
                            }
                        });
                });
            });
        }

        server_socket listen(socket_address sa) {
            return engine().listen(sa);
        }

        server_socket listen(socket_address sa, listen_options opts) {
            return engine().listen(sa, opts);
        }

        future<connected_socket> connect(socket_address sa) {
            return engine().connect(sa);
        }

        future<connected_socket> connect(socket_address sa, socket_address local, transport proto = transport::TCP) {
            return engine().connect(sa, local, proto);
        }

        socket make_socket() {
            return engine().net().socket();
        }

        net::udp_channel make_udp_channel() {
            return engine().net().make_udp_channel();
        }

        net::udp_channel make_udp_channel(const socket_address &local) {
            return engine().net().make_udp_channel(local);
        }

        void reactor::add_high_priority_task(task *t) noexcept {
            add_urgent_task(t);
            // break .then() chains
            request_preemption();
        }

        void set_idle_cpu_handler(idle_cpu_handler &&handler) {
            engine().set_idle_cpu_handler(std::move(handler));
        }

        static bool virtualized() {
            return boost::filesystem::exists("/sys/hypervisor/type");
        }

        std::chrono::nanoseconds reactor::calculate_poll_time() {
            // In a non-virtualized environment, select a poll time
            // that is competitive with halt/unhalt.
            //
            // In a virutalized environment, IPIs are slow and dominate
            // sleep/wake (mprotect/tgkill), so increase poll time to reduce
            // so we don't sleep in a request/reply workload
            return virtualized() ? 2000us : 200us;
        }

        future<> later() noexcept {
            memory::scoped_critical_alloc_section _;
            engine().force_poll();
            auto tsk = make_task(default_scheduling_group(), [] {});
            schedule(tsk);
            return tsk->get_future();
        }

        void add_to_flush_poller(output_stream<char> *os) {
            engine()._flush_batching.emplace_back(os);
        }

        reactor::sched_clock::duration reactor::total_idle_time() {
            return _total_idle;
        }

        reactor::sched_clock::duration reactor::total_busy_time() {
            return sched_clock::now() - _start_time - _total_idle;
        }

        std::chrono::nanoseconds reactor::total_steal_time() {
            // Steal time: this mimics the concept some Hypervisors have about Steal time.
            // That is the time in which a VM has something to run, but is not running because some other
            // process (another VM or the hypervisor itself) is in control.
            //
            // For us, we notice that during the time in which we were not sleeping (either running or busy
            // polling while idle), we should be accumulating thread runtime. If we are not, that's because
            // someone stole it from us.
            //
            // Because this is totally in userspace we can miss some events. For instance, if the actor
            // process is ready to run but the kernel hasn't scheduled us yet, that would be technically
            // steal time but we have no ways to account it.
            //
            // But what we have here should be good enough and at least has a well defined meaning.
            return std::chrono::duration_cast<std::chrono::nanoseconds>(sched_clock::now() - _start_time -
                                                                        _total_sleep) -
                   std::chrono::duration_cast<std::chrono::nanoseconds>(thread_cputime_clock::now().time_since_epoch());
        }

        static std::atomic<unsigned long> s_used_scheduling_group_ids_bitmap {3};    // 0=main, 1=atexit
        static std::atomic<unsigned long> s_next_scheduling_group_specific_key {0};

        static int allocate_scheduling_group_id() noexcept {
            static_assert(max_scheduling_groups() <= std::numeric_limits<unsigned long>::digits,
                          "more scheduling groups than available bits");
            auto b = s_used_scheduling_group_ids_bitmap.load(std::memory_order_relaxed);
            auto nb = b;
            unsigned i = 0;
            do {
                if (__builtin_popcountl(b) == max_scheduling_groups()) {
                    return -1;
                }
                i = count_trailing_zeros(~b);
                nb = b | (1ul << i);
            } while (!s_used_scheduling_group_ids_bitmap.compare_exchange_weak(b, nb, std::memory_order_relaxed));
            return i;
        }

        static unsigned long allocate_scheduling_group_specific_key() noexcept {
            return s_next_scheduling_group_specific_key.fetch_add(1, std::memory_order_relaxed);
        }

        static void deallocate_scheduling_group_id(unsigned id) noexcept {
            s_used_scheduling_group_ids_bitmap.fetch_and(~(1ul << id), std::memory_order_relaxed);
        }

        void reactor::allocate_scheduling_group_specific_data(scheduling_group sg, scheduling_group_key key) {
            auto &sg_data = _scheduling_group_specific_data;
            auto &this_sg = sg_data.per_scheduling_group_data[sg._id];
            this_sg.specific_vals.resize(std::max<size_t>(this_sg.specific_vals.size(), key.id() + 1));
            this_sg.specific_vals[key.id()] =
                aligned_alloc(sg_data.scheduling_group_key_configs[key.id()].alignment,
                              sg_data.scheduling_group_key_configs[key.id()].allocation_size);
            if (!this_sg.specific_vals[key.id()]) {
                std::abort();
            }
            if (sg_data.scheduling_group_key_configs[key.id()].constructor) {
                sg_data.scheduling_group_key_configs[key.id()].constructor(this_sg.specific_vals[key.id()]);
            }
        }

        future<> reactor::init_scheduling_group(nil::actor::scheduling_group sg, sstring name, float shares) {
            auto &sg_data = _scheduling_group_specific_data;
            auto &this_sg = sg_data.per_scheduling_group_data[sg._id];
            this_sg.queue_is_initialized = true;
            _task_queues.resize(std::max<size_t>(_task_queues.size(), sg._id + 1));
            _task_queues[sg._id] = std::make_unique<task_queue>(sg._id, name, shares);
            unsigned long num_keys = s_next_scheduling_group_specific_key.load(std::memory_order_relaxed);

            return with_scheduling_group(sg, [this, num_keys, sg]() {
                for (unsigned long key_id = 0; key_id < num_keys; key_id++) {
                    allocate_scheduling_group_specific_data(sg, scheduling_group_key(key_id));
                }
            });
        }

        future<> reactor::init_new_scheduling_group_key(scheduling_group_key key, scheduling_group_key_config cfg) {
            auto &sg_data = _scheduling_group_specific_data;
            sg_data.scheduling_group_key_configs.resize(
                std::max<size_t>(sg_data.scheduling_group_key_configs.size(), key.id() + 1));
            sg_data.scheduling_group_key_configs[key.id()] = cfg;
            return parallel_for_each(_task_queues, [this, cfg, key](std::unique_ptr<task_queue> &tq) {
                if (tq) {
                    scheduling_group sg = scheduling_group(tq->_id);
                    return with_scheduling_group(
                        sg, [this, key, sg]() { allocate_scheduling_group_specific_data(sg, key); });
                }
                return make_ready_future();
            });
        }

        future<> reactor::destroy_scheduling_group(scheduling_group sg) {
            return with_scheduling_group(sg,
                                         [this, sg]() {
                                             auto &sg_data = _scheduling_group_specific_data;
                                             auto &this_sg = sg_data.per_scheduling_group_data[sg._id];
                                             for (unsigned long key_id = 0;
                                                  key_id < sg_data.scheduling_group_key_configs.size();
                                                  key_id++) {
                                                 void *val = this_sg.specific_vals[key_id];
                                                 if (val) {
                                                     if (sg_data.scheduling_group_key_configs[key_id].destructor) {
                                                         sg_data.scheduling_group_key_configs[key_id].destructor(val);
                                                     }
                                                     free(val);
                                                     this_sg.specific_vals[key_id] = nullptr;
                                                 }
                                             }
                                         })
                .then([this, sg]() {
                    auto &sg_data = _scheduling_group_specific_data;
                    auto &this_sg = sg_data.per_scheduling_group_data[sg._id];
                    this_sg.queue_is_initialized = false;
                    _task_queues[sg._id].reset();
                });
        }

        void detail::no_such_scheduling_group(scheduling_group sg) {
            throw std::invalid_argument(
                format("The scheduling group does not exist ({})", detail::scheduling_group_index(sg)));
        }

        const sstring &scheduling_group::name() const noexcept {
            return engine()._task_queues[_id]->_name;
        }

        void scheduling_group::set_shares(float shares) noexcept {
            engine()._task_queues[_id]->set_shares(shares);
        }

        future<scheduling_group> create_scheduling_group(sstring name, float shares) noexcept {
            auto aid = allocate_scheduling_group_id();
            if (aid < 0) {
                return make_exception_future<scheduling_group>(std::runtime_error("Scheduling group limit exceeded"));
            }
            auto id = static_cast<unsigned>(aid);
            assert(id < max_scheduling_groups());
            auto sg = scheduling_group(id);
            return smp::invoke_on_all([sg, name, shares] { return engine().init_scheduling_group(sg, name, shares); })
                .then([sg] { return make_ready_future<scheduling_group>(sg); });
        }

        future<scheduling_group_key> scheduling_group_key_create(scheduling_group_key_config cfg) noexcept {
            scheduling_group_key key = allocate_scheduling_group_specific_key();
            return smp::invoke_on_all([key, cfg] { return engine().init_new_scheduling_group_key(key, cfg); })
                .then([key] { return make_ready_future<scheduling_group_key>(key); });
        }

        future<> rename_priority_class(io_priority_class pc, sstring new_name) {
            return reactor::rename_priority_class(pc, new_name);
        }

        future<> destroy_scheduling_group(scheduling_group sg) noexcept {
            if (sg == default_scheduling_group()) {
                return make_exception_future<>(make_backtraced_exception_ptr<std::runtime_error>(
                    "Attempt to destroy the default scheduling group"));
            }
            if (sg == current_scheduling_group()) {
                return make_exception_future<>(make_backtraced_exception_ptr<std::runtime_error>(
                    "Attempt to destroy the current scheduling group"));
            }
            return smp::invoke_on_all([sg] { return engine().destroy_scheduling_group(sg); }).then([sg] {
                deallocate_scheduling_group_id(sg._id);
            });
        }

        future<> rename_scheduling_group(scheduling_group sg, sstring new_name) noexcept {
            if (sg == default_scheduling_group()) {
                return make_exception_future<>(make_backtraced_exception_ptr<std::runtime_error>(
                    "Attempt to rename the default scheduling group"));
            }
            return smp::invoke_on_all([sg, new_name] { engine()._task_queues[sg._id]->rename(new_name); });
        }

        namespace detail {

            inline std::chrono::steady_clock::duration timeval_to_duration(::timeval tv) {
                return std::chrono::seconds(tv.tv_sec) + std::chrono::microseconds(tv.tv_usec);
            }

            class reactor_stall_sampler : public reactor::pollfn {
                std::chrono::steady_clock::time_point _run_start;
                ::rusage _run_start_rusage;
                uint64_t _kernel_stalls = 0;
                std::chrono::steady_clock::duration _nonsleep_cpu_time = {};
                std::chrono::steady_clock::duration _nonsleep_wall_time = {};

            private:
                static ::rusage get_rusage() {
                    struct ::rusage ru;
#if BOOST_OS_LINUX
                    ::getrusage(RUSAGE_THREAD, &ru);
#elif BOOST_OS_MACOS || BOOST_OS_IOS
                    detail::getrusage_thread(&ru);
#endif
                    return ru;
                }
                static std::chrono::steady_clock::duration cpu_time(const ::rusage &ru) {
                    return timeval_to_duration(ru.ru_stime) + timeval_to_duration(ru.ru_utime);
                }
                void mark_run_start() {
                    _run_start = std::chrono::steady_clock::now();
                    _run_start_rusage = get_rusage();
                }
                void mark_run_end() {
                    auto start_nvcsw = _run_start_rusage.ru_nvcsw;
                    auto start_cpu_time = cpu_time(_run_start_rusage);
                    auto start_time = _run_start;
                    _run_start = std::chrono::steady_clock::now();
                    _run_start_rusage = get_rusage();
                    _kernel_stalls += _run_start_rusage.ru_nvcsw - start_nvcsw;
                    _nonsleep_cpu_time += cpu_time(_run_start_rusage) - start_cpu_time;
                    _nonsleep_wall_time += _run_start - start_time;
                }

            public:
                reactor_stall_sampler() {
                    mark_run_start();
                }
                virtual bool poll() override {
                    return false;
                }
                virtual bool pure_poll() override {
                    return false;
                }
                virtual bool try_enter_interrupt_mode() override {
                    // try_enter_interrupt_mode marks the end of a reactor run that should be context-switch free
                    mark_run_end();
                    return true;
                }
                virtual void exit_interrupt_mode() override {
                    // start a reactor run that should be context switch free
                    mark_run_start();
                }
                stall_report report() const {
                    stall_report r;
                    // mark_run_end() with an immediate mark_run_start() is logically a no-op,
                    // but each one of them has an effect, so they can't be marked const
                    const_cast<reactor_stall_sampler *>(this)->mark_run_end();
                    r.kernel_stalls = _kernel_stalls;
                    r.run_wall_time = _nonsleep_wall_time;
                    r.stall_time = _nonsleep_wall_time - _nonsleep_cpu_time;
                    const_cast<reactor_stall_sampler *>(this)->mark_run_start();
                    return r;
                }
            };

            future<stall_report> report_reactor_stalls(noncopyable_function<future<>()> uut) {
                auto reporter = std::make_unique<reactor_stall_sampler>();
                auto p_reporter = reporter.get();
                auto poller = reactor::poller(std::move(reporter));
                return uut().then([poller = std::move(poller), p_reporter]() mutable { return p_reporter->report(); });
            }

            std::ostream &operator<<(std::ostream &os, const stall_report &sr) {
                auto to_ms = [](std::chrono::steady_clock::duration d) -> float {
                    return std::chrono::duration<float>(d) / 1ms;
                };
                return os << format("{} stalls, {} ms stall time, {} ms run time", sr.kernel_stalls,
                                    to_ms(sr.stall_time), to_ms(sr.run_wall_time));
            }

        }    // namespace detail

#ifdef ACTOR_TASK_BACKTRACE

        void task::make_backtrace() noexcept {
            memory::disable_backtrace_temporarily dbt;
            try {
                _bt = make_lw_shared<simple_backtrace>(current_backtrace_tasklocal());
            } catch (...) {
                _bt = nullptr;
            }
        }

#endif

    }    // namespace actor
}    // namespace nil
