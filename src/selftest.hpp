#pragma once

#include "core/algorithm.hpp"

namespace pastiche {

// Runs every registered algorithm on small synthetic images and checks the
// output (dimensions, not uniform, no failure). Prints a report to stdout.
// Returns the process exit code (0 = all passed).
int run_selftest(const RunOptions& opts);

// Times every registered algorithm at a few resolutions. Returns exit code.
int run_benchmark(const RunOptions& opts);

} // namespace pastiche
