#pragma once

#include <vector>

// The fastest cores, read from the kernel, for pinning the client: on hybrid CPUs
// (Intel P/E, ARM big.LITTLE, AMD full/compact) the scheduler may put its main
// thread on a slow one, which macOS avoids by priority.

namespace stud::ui::cpu {

// Empty when every core is the same kind.
std::vector<int> fastest();

// Cores, not SMT threads, among `cpus`; every online core when it is empty.
int physical_cores(const std::vector<int>& cpus);

// Processors among `cpus`; every online one when it is empty.
int logical_cpus(const std::vector<int>& cpus);

}  // namespace stud::ui::cpu
