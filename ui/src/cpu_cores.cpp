#include "cpu_cores.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace stud::ui::cpu {
namespace {

const std::string kCpus = "/sys/devices/system/cpu";

std::string read(const std::string& path) {
    std::ifstream file(path);
    std::string text;
    std::getline(file, text);
    return text;
}

// e.g. "0-3,8,10-11".
std::vector<int> parse_list(const std::string& text) {
    std::vector<int> cpus;
    std::stringstream parts(text);
    std::string part;
    while (std::getline(parts, part, ',')) {
        if (part.empty()) continue;
        const auto dash = part.find('-');
        try {
            const int first = std::stoi(part.substr(0, dash));
            const int last = dash == std::string::npos ? first : std::stoi(part.substr(dash + 1));
            for (int cpu = first; cpu <= last; ++cpu) cpus.push_back(cpu);
        } catch (const std::exception&) {
            return {};
        }
    }
    return cpus;
}

std::vector<int> online() { return parse_list(read(kCpus + "/online")); }

long long number(const std::string& path) {
    try {
        const std::string text = read(path);
        return text.empty() ? -1 : std::stoll(text);
    } catch (const std::exception&) {
        return -1;
    }
}

std::string cpu_dir(int cpu) { return kCpus + "/cpu" + std::to_string(cpu); }

// Intel hybrid: the performance cores' PMU is `cpu_core`, the efficiency cores' `cpu_atom`.
std::vector<int> intel_performance_cores(const std::vector<int>& all) {
    std::vector<int> cores = parse_list(read("/sys/devices/cpu_core/cpus"));
    if (cores.empty() || cores.size() >= all.size()) return {};
    return cores;
}

// ARM big.LITTLE: the cores with the highest capacity.
std::vector<int> highest_capacity(const std::vector<int>& all) {
    std::map<int, long long> capacity;
    for (const int cpu : all) {
        const long long value = number(cpu_dir(cpu) + "/cpu_capacity");
        if (value < 0) return {};
        capacity[cpu] = value;
    }
    long long best = 0;
    for (const auto& [cpu, value] : capacity) best = std::max(best, value);
    std::vector<int> cores;
    for (const auto& [cpu, value] : capacity) {
        if (value == best) cores.push_back(cpu);
    }
    return cores.size() < all.size() ? cores : std::vector<int>{};
}

// "32768K" or "96M" in bytes; -1 when unreadable.
long long cache_bytes(const std::string& text) {
    try {
        std::size_t used = 0;
        const long long value = std::stoll(text, &used);
        const char unit = used < text.size() ? text[used] : '\0';
        return unit == 'K' ? value << 10 : unit == 'M' ? value << 20 : unit == 'G' ? value << 30 : value;
    } catch (const std::exception&) {
        return -1;
    }
}

// Otherwise the last-level cache group with the most cache (AMD X3D's V-Cache, faster
// for games despite lower clocks), or the fastest cores when caches are alike.
std::vector<int> fastest_cache_group(const std::vector<int>& all) {
    std::map<std::string, std::vector<int>> groups;
    std::map<std::string, long long> cache;
    for (const int cpu : all) {
        std::string shared;
        std::string size;
        for (int index = 0;; ++index) {
            const std::string dir = cpu_dir(cpu) + "/cache/index" + std::to_string(index);
            const std::string level = read(dir + "/shared_cpu_list");
            if (level.empty()) break;
            shared = level;
            size = read(dir + "/size");
        }
        if (shared.empty()) return {};
        groups[shared].push_back(cpu);
        cache[shared] = cache_bytes(size);
    }
    if (groups.size() < 2) return {};
    long long least_cache = -1, most_cache = -1;
    for (const auto& [key, bytes] : cache) {
        least_cache = least_cache < 0 ? bytes : std::min(least_cache, bytes);
        most_cache = std::max(most_cache, bytes);
    }
    const bool by_cache = least_cache > 0 && least_cache != most_cache;
    std::map<std::string, long long> peak;
    for (const auto& [key, cpus] : groups) {
        if (by_cache) {
            peak[key] = cache[key];
            continue;
        }
        for (const int cpu : cpus) {
            peak[key] = std::max(peak[key], number(cpu_dir(cpu) + "/cpufreq/cpuinfo_max_freq"));
        }
    }
    long long best = 0;
    for (const auto& [key, measure] : peak) best = std::max(best, measure);
    if (best <= 0) return {};
    std::vector<int> cores;
    for (const auto& [key, cpus] : groups) {
        if (peak[key] == best) cores.insert(cores.end(), cpus.begin(), cpus.end());
    }
    std::sort(cores.begin(), cores.end());
    return cores.size() < all.size() ? cores : std::vector<int>{};
}

}  // namespace

std::vector<int> fastest() {
    const std::vector<int> all = online();
    if (all.empty()) return {};
    for (auto find : {intel_performance_cores, highest_capacity, fastest_cache_group}) {
        if (std::vector<int> cores = find(all); !cores.empty()) return cores;
    }
    return {};
}

int logical_cpus(const std::vector<int>& cpus) {
    return static_cast<int>(cpus.empty() ? online().size() : cpus.size());
}

int physical_cores(const std::vector<int>& cpus) {
    const std::vector<int> chosen = cpus.empty() ? online() : cpus;
    std::set<std::string> cores;
    for (const int cpu : chosen) {
        const std::string siblings = read(cpu_dir(cpu) + "/topology/thread_siblings_list");
        cores.insert(siblings.empty() ? std::to_string(cpu) : siblings);
    }
    return static_cast<int>(cores.size());
}

}  // namespace stud::ui::cpu
