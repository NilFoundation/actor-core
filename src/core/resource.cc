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

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>

#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/copy.hpp>

#include <regex>

#include <nil/actor/core/resource.hh>
#include <nil/actor/core/align.hh>
#include <nil/actor/core/print.hh>
#include <nil/actor/detail/read_first_line.hh>

#include <cstdlib>
#include <limits>

#include <nil/actor/core/detail/cgroup.hh>
#include <nil/actor/detail/log.hh>

namespace nil {
    namespace actor {

        extern logger actor_logger;

        // This function was made optional because of validate. It needs to
        // throw an error when a non parseable input is given.
        boost::optional<resource::cpuset> parse_cpuset(std::string value) {
            static std::regex r("(\\d+-)?(\\d+)(,(\\d+-)?(\\d+))*");

            std::smatch match;
            if (std::regex_match(value, match, r)) {
                std::vector<std::string> ranges;
                boost::split(ranges, value, boost::is_any_of(","));
                resource::cpuset ret;
                for (auto &&range : ranges) {
                    std::string beg = range;
                    std::string end = range;
                    auto dash = range.find('-');
                    if (dash != range.npos) {
                        beg = range.substr(0, dash);
                        end = range.substr(dash + 1);
                    }
                    auto b = boost::lexical_cast<unsigned>(beg);
                    auto e = boost::lexical_cast<unsigned>(end);

                    if (b > e) {
                        return boost::none;
                    }

                    for (auto i = b; i <= e; ++i) {
                        ret.insert(i);
                    }
                }
                return ret;
            }
            return boost::none;
        }

        // Overload for boost program options parsing/validation
        void validate(boost::any &v, const std::vector<std::string> &values, cpuset_bpo_wrapper *target_type, int) {
            using namespace boost::program_options;
            validators::check_first_occurrence(v);

            // Extract the first string from 'values'. If there is more than
            // one string, it's an error, and exception will be thrown.
            auto &&s = validators::get_single_string(values);
            auto parsed_cpu_set = parse_cpuset(s);

            if (parsed_cpu_set) {
                cpuset_bpo_wrapper ret;
                ret.value = *parsed_cpu_set;
                v = std::move(ret);
            } else {
                throw validation_error(validation_error::invalid_option_value);
            }
        }

        namespace cgroup {

            namespace fs = boost::filesystem;

            optional<cpuset> cpu_set() {
#if BOOST_OS_LINUX
                auto cpuset = read_setting_V1V2_as<std::string>("cpuset/cpuset.cpus", "cpuset.cpus.effective");
                if (cpuset) {
                    return nil::actor::parse_cpuset(*cpuset);
                }

                actor_logger.warn("Unable to parse cgroup's cpuset. Ignoring.");
#endif
                return boost::none;
            }

            size_t memory_limit() {
#if BOOST_OS_LINUX
                return read_setting_V1V2_as<size_t>("memory/memory.limit_in_bytes", "memory.max")
                    .value_or(std::numeric_limits<size_t>::max());
#else
                return std::numeric_limits<size_t>::max();
#endif
            }

            template<typename T>
            optional<T> read_setting_as(std::string path) {
                try {
                    auto line = read_first_line(path);
                    return boost::lexical_cast<T>(line);
                } catch (...) {
                    actor_logger.warn("Couldn't read cgroup file {}.", path);
                }

                return boost::none;
            }

#if BOOST_OS_LINUX
            /*
             * what cgroup do we belong to?
             *
             * For cgroups V2, /proc/self/cgroup should read "0::<cgroup-dir-path>"
             * Note: true only for V2-only systems, but there is no reason to support
             * a hybrid configuration.
             */
            static optional<boost::filesystem::path> cgroup2_path_my_pid() {
                nil::actor::sstring cline;
                try {
                    cline = read_first_line(boost::filesystem::path {"/proc/self/cgroup"});
                } catch (...) {
                    // '/proc/self/cgroup' must be there. If not - there is an issue
                    // with the system configuration.
                    throw std::runtime_error("no cgroup data for our process");
                }

                // for a V2-only system, we expect exactly one line:
                // 0::<abs-path-to-cgroup>
                if (cline.at(0) != '0') {
                    // This is either a v1 system, or system configured with a hybrid of v1 & v2.
                    // We do not support such combinations of v1 and v2 at this point.
                    actor_logger.debug("Not a cgroups-v2-only system");
                    return boost::none;
                }

                // the path is guaranteed to start with '0::/'
                return boost::filesystem::path {"/sys/fs/cgroup/" + cline.substr(4)};
            }

            /*
             * traverse the cgroups V2 hierarchy bottom-up, starting from our process'
             * specific cgroup up to /sys/fs/cgroup, looking for the named file.
             */
            static optional<boost::filesystem::path> locate_lowest_cgroup2(boost::filesystem::path lowest_subdir, std::string filename) {
                // locate the lowest subgroup containing the named file (i.e.
                // handles the requested control by itself)
                do {
                    //  does the cgroup settings file exist?
                    auto set_path = lowest_subdir / filename;
                    if (boost::filesystem::exists(set_path)) {
                        return set_path;
                    }

                    lowest_subdir = lowest_subdir.parent_path();
                } while (lowest_subdir.compare("/sys/fs"));

                return boost::none;
            }

            /*
             * Read a settings value from either the cgroups V2 or the corresponding
             * cgroups V1 files.
             * For V2, look for the lowest cgroup in our hierarchy that manages the
             * requested settings.
             */
            template<typename T>
            optional<T> read_setting_V1V2_as(std::string cg1_path, std::string cg2_fname) {
                // on v2-systems, cg2_path will be initialized with the leaf cgroup that
                // controls this process
                static optional<boost::filesystem::path> cg2_path {cgroup2_path_my_pid()};

                if (cg2_path) {
                    // this is a v2 system
                    nil::actor::sstring line;
                    try {
                        line = read_first_line(locate_lowest_cgroup2(*cg2_path, cg2_fname).value());
                    } catch (...) {
                        actor_logger.warn("Could not read cgroups v2 file ({}).", cg2_fname);
                        return boost::none;
                    }
                    if (line.compare("max")) {
                        try {
                            return boost::lexical_cast<T>(line);
                        } catch (...) {
                            actor_logger.warn("Malformed cgroups file ({}) contents.", cg2_fname);
                        }
                    }
                    return boost::none;
                }

                // try cgroups v1:
                try {
                    auto line = read_first_line(boost::filesystem::path {"/sys/fs/cgroup"} / cg1_path);
                    return boost::lexical_cast<T>(line);
                } catch (...) {
                    actor_logger.warn("Could not parse cgroups v1 file ({}).", cg1_path);
                }

                return boost::none;
            }
#endif
        }    // namespace cgroup

        namespace resource {

            size_t calculate_memory(configuration c, size_t available_memory, float panic_factor = 1) {
                size_t default_reserve_memory =
                    std::max<size_t>(1536 * 1024 * 1024, 0.07 * available_memory) * panic_factor;
                auto reserve = c.reserve_memory.value_or(default_reserve_memory);
                size_t min_memory = 500'000'000;
                if (available_memory >= reserve + min_memory) {
                    available_memory -= reserve;
                } else {
                    // Allow starting up even in low memory configurations (e.g. 2GB boot2docker VM)
                    available_memory = min_memory;
                }
                size_t mem = c.total_memory.value_or(available_memory);
                if (mem > available_memory) {
                    throw std::runtime_error(
                        format("insufficient physical memory: needed {} available {}", mem, available_memory));
                }
                return mem;
            }

        }    // namespace resource

    }    // namespace actor
}    // namespace nil

#ifdef ACTOR_HAVE_HWLOC

#include <nil/actor/detail/defer.hh>
#include <nil/actor/core/print.hh>

#include <hwloc.h>
#include <unordered_map>
#include <algorithm>

#include <boost/range/irange.hpp>

namespace nil {
    namespace actor {

        cpu_set_t cpuid_to_cpuset(unsigned cpuid) {
            cpu_set_t cs;
            CPU_ZERO(&cs);
            CPU_SET(cpuid, &cs);
            return cs;
        }

        namespace resource {

            size_t div_roundup(size_t num, size_t denom) {
                return (num + denom - 1) / denom;
            }

            static size_t alloc_from_node(cpu &this_cpu, hwloc_obj_t node,
                                          std::unordered_map<hwloc_obj_t, size_t> &used_mem, size_t alloc) {
#if HWLOC_API_VERSION >= 0x00020000
                // FIXME: support nodes with multiple NUMA nodes, whatever that means
                auto local_memory = node->total_memory;
#else
                auto local_memory = node->memory.local_memory;
#endif
                auto taken = std::min(local_memory - used_mem[node], static_cast<uint64_t>(alloc));
                if (taken) {
                    used_mem[node] += taken;
                    auto node_id = hwloc_bitmap_first(node->nodeset);
                    assert(node_id != -1);
                    this_cpu.mem.push_back({taken, unsigned(node_id)});
                }
                return taken;
            }

            // Find the numa node that contains a specific PU.
            static hwloc_obj_t get_numa_node_for_pu(hwloc_topology_t &topology, hwloc_obj_t pu) {
                // Can't use ancestry because hwloc 2.0 NUMA nodes are not ancestors of PUs
                hwloc_obj_t tmp = NULL;
                auto depth = hwloc_get_type_or_above_depth(topology, HWLOC_OBJ_NUMANODE);
                while ((tmp = hwloc_get_next_obj_by_depth(topology, depth, tmp)) != NULL) {
                    if (hwloc_bitmap_intersects(tmp->cpuset, pu->cpuset)) {
                        return tmp;
                    }
                }
                return nullptr;
            }

            static hwloc_obj_t hwloc_get_ancestor(hwloc_obj_type_t type, hwloc_topology_t &topology, unsigned cpu_id) {
                auto cur = hwloc_get_pu_obj_by_os_index(topology, cpu_id);

                while (cur != nullptr) {
                    if (cur->type == type) {
                        break;
                    }
                    cur = cur->parent;
                }

                return cur;
            }

            static std::unordered_map<hwloc_obj_t, std::vector<unsigned>>
                break_cpus_into_groups(hwloc_topology_t &topology, std::vector<unsigned> cpus, hwloc_obj_type_t type) {
                std::unordered_map<hwloc_obj_t, std::vector<unsigned>> groups;

                for (auto &&cpu_id : cpus) {
                    hwloc_obj_t anc = hwloc_get_ancestor(type, topology, cpu_id);
                    groups[anc].push_back(cpu_id);
                }

                return groups;
            }

            struct distribute_objects {
                std::vector<hwloc_cpuset_t> cpu_sets;
                hwloc_obj_t root;

                distribute_objects(hwloc_topology_t &topology, size_t nobjs) :
                    cpu_sets(nobjs), root(hwloc_get_root_obj(topology)) {
#if HWLOC_API_VERSION >= 0x00010900
                    hwloc_distrib(topology, &root, 1, cpu_sets.data(), cpu_sets.size(), INT_MAX, 0);
#else
                    hwloc_distribute(topology, root, cpu_sets.data(), cpu_sets.size(), INT_MAX);
#endif
                }

                ~distribute_objects() {
                    for (auto &&cs : cpu_sets) {
                        hwloc_bitmap_free(cs);
                    }
                }
                std::vector<hwloc_cpuset_t> &operator()() {
                    return cpu_sets;
                }
            };

            static io_queue_topology allocate_io_queues(hwloc_topology_t &topology, std::vector<cpu> cpus,
                                                        std::unordered_map<unsigned, hwloc_obj_t> &cpu_to_node,
                                                        unsigned num_io_groups, unsigned &last_node_idx) {
                auto node_of_shard = [&cpus, &cpu_to_node](unsigned shard) {
                    auto node = cpu_to_node.at(cpus[shard].cpu_id);
                    return hwloc_bitmap_first(node->nodeset);
                };

                // There are two things we are trying to achieve by populating a numa_nodes map.
                //
                // The first is to find out how many nodes we have in the system. We can't use
                // hwloc for that, because at this point we are not longer talking about the physical system,
                // but the actual booted actor server instead. So if we have restricted the run to a subset
                // of the available processors, counting topology nodes won't spur the same result.
                //
                // Secondly, we need to find out which processors live in each node. For a reason similar to the
                // above, hwloc won't do us any good here. Later on, we will use this information to assign
                // shards to coordinators that are node-local to themselves.
                std::unordered_map<unsigned, std::set<unsigned>> numa_nodes;
                for (auto shard : boost::irange(0, int(cpus.size()))) {
                    auto node_id = node_of_shard(shard);

                    if (numa_nodes.count(node_id) == 0) {
                        numa_nodes.emplace(node_id, std::set<unsigned>());
                    }
                    numa_nodes.at(node_id).insert(shard);
                }

                io_queue_topology ret;
                ret.shard_to_group.resize(cpus.size());

                if (num_io_groups == 0) {
                    num_io_groups = numa_nodes.size();
                    assert(num_io_groups != 0);
                    actor_logger.debug("Auto-configure {} IO groups", num_io_groups);
                } else if (num_io_groups > cpus.size()) {
                    // User may be playing with --smp option, but num_io_groups was independently
                    // determined by iotune, so adjust for any conflicts.
                    fmt::print(
                        "Warning: number of IO queues ({:d}) greater than logical cores ({:d}). Adjusting downwards.\n",
                        num_io_groups, cpus.size());
                    num_io_groups = cpus.size();
                }

                auto find_shard = [&cpus](unsigned cpu_id) {
                    auto idx = 0u;
                    for (auto &c : cpus) {
                        if (c.cpu_id == cpu_id) {
                            return idx;
                        }
                        idx++;
                    }
                    assert(0);
                };

                auto cpu_sets = distribute_objects(topology, num_io_groups);
                ret.nr_queues = cpus.size();
                ret.nr_groups = 0;

                // First step: distribute the IO queues given the information returned in cpu_sets.
                // If there is one IO queue per processor, only this loop will be executed.
                std::unordered_map<unsigned, std::vector<unsigned>> node_coordinators;
                for (auto &&cs : cpu_sets()) {
                    auto io_coordinator = find_shard(hwloc_bitmap_first(cs));
                    ret.shard_to_group[io_coordinator] = ret.nr_groups++;

                    auto node_id = node_of_shard(io_coordinator);
                    if (node_coordinators.count(node_id) == 0) {
                        node_coordinators.emplace(node_id, std::vector<unsigned>());
                    }
                    node_coordinators.at(node_id).push_back(io_coordinator);
                    numa_nodes[node_id].erase(io_coordinator);
                }

                auto available_nodes =
                    boost::copy_range<std::vector<unsigned>>(node_coordinators | boost::adaptors::map_keys);

                // If there are more processors than coordinators, we will have to assign them to existing
                // coordinators. We prefer do that within the same NUMA node, but if not possible we assign
                // the shard to a random node.
                for (auto &node : numa_nodes) {
                    auto cid_idx = 0;
                    for (auto &remaining_shard : node.second) {
                        auto my_node = node.first;
                        // No I/O queue in this node, round-robin shards from this node into existing ones.
                        if (!node_coordinators.count(node.first)) {
                            my_node = available_nodes[last_node_idx++ % available_nodes.size()];
                        }
                        auto idx = cid_idx++ % node_coordinators.at(my_node).size();
                        auto io_coordinator = node_coordinators.at(my_node)[idx];
                        ret.shard_to_group[remaining_shard] = ret.shard_to_group[io_coordinator];
                    }
                }

                return ret;
            }

            resources allocate(configuration c) {
                hwloc_topology_t topology;
                hwloc_topology_init(&topology);
                auto free_hwloc = defer([&] { hwloc_topology_destroy(topology); });
                hwloc_topology_load(topology);
                if (c.cpu_set) {
                    auto bm = hwloc_bitmap_alloc();
                    auto free_bm = defer([&] { hwloc_bitmap_free(bm); });
                    for (auto idx : *c.cpu_set) {
                        hwloc_bitmap_set(bm, idx);
                    }
                    auto r =
                        hwloc_topology_restrict(topology, bm,
#if HWLOC_API_VERSION >= 0x00020000
                                                0
#else
                                                HWLOC_RESTRICT_FLAG_ADAPT_DISTANCES
#endif
                                                    | HWLOC_RESTRICT_FLAG_ADAPT_MISC | HWLOC_RESTRICT_FLAG_ADAPT_IO);
                    if (r == -1) {
                        if (errno == ENOMEM) {
                            throw std::bad_alloc();
                        }
                        if (errno == EINVAL) {
                            throw std::runtime_error("bad cpuset");
                        }
                        abort();
                    }
                }
                auto machine_depth = hwloc_get_type_depth(topology, HWLOC_OBJ_MACHINE);
                assert(hwloc_get_nbobjs_by_depth(topology, machine_depth) == 1);
                auto machine = hwloc_get_obj_by_depth(topology, machine_depth, 0);
#if HWLOC_API_VERSION >= 0x00020000
                auto available_memory = machine->total_memory;
#else
                auto available_memory = machine->memory.total_memory;
#endif
                size_t mem =
                    calculate_memory(c, std::min(available_memory, static_cast<uint64_t>(cgroup::memory_limit())));
                unsigned available_procs = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
                unsigned procs = c.cpus.value_or(available_procs);
                if (procs > available_procs) {
                    throw std::runtime_error("insufficient processing units");
                }
                // limit memory address to fit in 36-bit, see core/memory.cc:Memory map
                constexpr size_t max_mem_per_proc = 1UL << 38;
                auto mem_per_proc = std::min(align_down<size_t>(mem / (procs + 48 * 16 - 1), 2 << 20), max_mem_per_proc);
                resources ret;
                std::unordered_map<unsigned, hwloc_obj_t> cpu_to_node;
                std::vector<unsigned> orphan_pus;
                std::unordered_map<hwloc_obj_t, size_t> topo_used_mem;
                std::vector<std::pair<cpu, size_t>> remains;
                size_t remain;

                auto cpu_sets = distribute_objects(topology, procs);

                for (auto &&cs : cpu_sets()) {
                    auto cpu_id = hwloc_bitmap_first(cs);
                    assert(cpu_id != -1);
                    auto pu = hwloc_get_pu_obj_by_os_index(topology, cpu_id);
                    auto node = get_numa_node_for_pu(topology, pu);
                    if (node == nullptr) {
                        orphan_pus.push_back(cpu_id);
                    } else {
                        cpu_to_node[cpu_id] = node;
                        actor_logger.debug("Assign CPU{} to NUMA{}", cpu_id, node->os_index);
                    }
                }

                if (!orphan_pus.empty()) {
                    if (!c.assign_orphan_cpus) {
                        actor_logger.error(
                            "CPUs without local NUMA nodes are disabled by the "
                            "--allow-cpus-in-remote-numa-nodes=false option.\n");
                        throw std::runtime_error("no NUMA node for CPU");
                    }

                    actor_logger.warn("Assigning some CPUs to remote NUMA nodes");

                    // Get the list of NUMA nodes available
                    std::vector<hwloc_obj_t> nodes;

                    hwloc_obj_t tmp = NULL;
                    auto depth = hwloc_get_type_or_above_depth(topology, HWLOC_OBJ_NUMANODE);
                    while ((tmp = hwloc_get_next_obj_by_depth(topology, depth, tmp)) != NULL) {
                        nodes.push_back(tmp);
                    }

                    // Group orphan CPUs by ... some sane enough feature
                    std::unordered_map<hwloc_obj_t, std::vector<unsigned>> grouped;
                    hwloc_obj_type_t group_by[] = {
                        HWLOC_OBJ_L3CACHE,
                        HWLOC_OBJ_L2CACHE,
                        HWLOC_OBJ_L1CACHE,
                        HWLOC_OBJ_PU,
                    };

                    for (auto &&gb : group_by) {
                        grouped = break_cpus_into_groups(topology, orphan_pus, gb);
                        if (grouped.size() >= nodes.size()) {
                            actor_logger.debug("Grouped orphan CPUs by {}", hwloc_obj_type_string(gb));
                            break;
                        }
                        // Try to scatter orphans into as much NUMA nodes as possible
                        // by grouping them with more specific selection
                    }

                    // Distribute PUs among the nodes by groups
                    unsigned nid = 0;
                    for (auto &&grp : grouped) {
                        for (auto &&cpu_id : grp.second) {
                            cpu_to_node[cpu_id] = nodes[nid];
                            actor_logger.debug("Assign orphan CPU{} to NUMA{}", cpu_id, nodes[nid]->os_index);
                        }
                        nid = (nid + 1) % nodes.size();
                    }
                }

                // Divide local memory to cpus
                for (auto &&cs : cpu_sets()) {
                    auto cpu_id = hwloc_bitmap_first(cs);
                    assert(cpu_id != -1);
                    auto node = cpu_to_node.at(cpu_id);
                    cpu this_cpu;
                    this_cpu.cpu_id = cpu_id;
                    if (cpu_id == 0) {
                        remain = mem_per_proc * 48 * 16 - alloc_from_node(this_cpu, node, topo_used_mem, mem_per_proc * 48 * 16);
                    } else {
                        remain = mem_per_proc - alloc_from_node(this_cpu, node, topo_used_mem, mem_per_proc);
                    }
                    remains.emplace_back(std::move(this_cpu), remain);
                }

                // Divide the rest of the memory
                auto depth = hwloc_get_type_or_above_depth(topology, HWLOC_OBJ_NUMANODE);
                for (auto &&r : remains) {
                    cpu this_cpu;
                    size_t remain;
                    std::tie(this_cpu, remain) = r;
                    auto node = cpu_to_node.at(this_cpu.cpu_id);
                    auto obj = node;

                    while (remain) {
                        remain -= alloc_from_node(this_cpu, obj, topo_used_mem, remain);
                        do {
                            obj = hwloc_get_next_obj_by_depth(topology, depth, obj);
                        } while (!obj);
                        if (obj == node)
                            break;
                    }
                    assert(!remain);
                    ret.cpus.push_back(std::move(this_cpu));
                }

                unsigned last_node_idx = 0;
                for (auto devid : c.devices) {
                    ret.ioq_topology.emplace(
                        devid, allocate_io_queues(topology, ret.cpus, cpu_to_node, c.num_io_groups, last_node_idx));
                }
                return ret;
            }

            unsigned nr_processing_units() {
                hwloc_topology_t topology;
                hwloc_topology_init(&topology);
                auto free_hwloc = defer([&] { hwloc_topology_destroy(topology); });
                hwloc_topology_load(topology);
                return hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
            }

        }    // namespace resource

    }    // namespace actor
}    // namespace nil

#else

#include <nil/actor/core/resource.hh>
#include <unistd.h>

namespace nil {
    namespace actor {

        namespace resource {

            // Without hwloc, we don't support tuning the number of IO queues. So each CPU gets their.
            static io_queue_topology allocate_io_queues(configuration c, std::vector<cpu> cpus) {
                io_queue_topology ret;

                unsigned nr_cpus = unsigned(cpus.size());
                ret.nr_queues = nr_cpus;
                ret.shard_to_group.resize(nr_cpus);
                ret.nr_groups = 1;

                for (unsigned shard = 0; shard < nr_cpus; ++shard) {
                    ret.shard_to_group[shard] = 0;
                }
                return ret;
            }

            resources allocate(configuration c) {
                resources ret;

                auto available_memory = ::sysconf(_SC_PAGESIZE) * size_t(::sysconf(_SC_PHYS_PAGES));
                auto mem = calculate_memory(c, available_memory);
                auto cpuset_procs = c.cpu_set ? c.cpu_set->size() : nr_processing_units();
                auto procs = c.cpus.value_or(cpuset_procs);
                ret.cpus.reserve(procs);
                if (c.cpu_set) {
                    for (auto cpuid : *c.cpu_set) {
                        ret.cpus.push_back(cpu {cpuid, {{mem / procs, 0}}});
                    }
                } else {
                    for (unsigned i = 0; i < procs; ++i) {
                        ret.cpus.push_back(cpu {i, {{mem / procs, 0}}});
                    }
                }

                ret.ioq_topology.emplace(0, allocate_io_queues(c, ret.cpus));
                return ret;
            }

            unsigned nr_processing_units() {
                return ::sysconf(_SC_NPROCESSORS_ONLN);
            }

        }    // namespace resource

    }    // namespace actor
}    // namespace nil

#endif
