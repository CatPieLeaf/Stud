#pragma once

#include <vector>

// The machine's fastest processors, for running the client on them.
//
// On a CPU with more than one kind of core (Intel's performance, efficiency
// and low-power efficiency cores, ARM's big.LITTLE, AMD's full and compact
// cores) the scheduler may put the client's main thread on a slow core; on
// a Mac, the main thread's priority keeps it on a fast one. Which cores are
// the fast ones is read from what the kernel reports, never assumed.

namespace stud::ui::cpu {

// The processors of the fastest kind, or empty when every processor is of
// one kind and nothing should be restricted.
std::vector<int> fastest();

// Cores, not SMT threads, among `cpus`; every online core when it is empty.
int physical_cores(const std::vector<int>& cpus);

// Processors among `cpus`; every online one when it is empty.
int logical_cpus(const std::vector<int>& cpus);

}  // namespace stud::ui::cpu
