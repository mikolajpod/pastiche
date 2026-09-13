#pragma once

#include <string>
#include <vector>

namespace pastiche {

// `pastiche download [--list | --verify | <name>...]`.
//
// Handles the model catalogue subcommand end to end: listing what is available,
// showing a licence and asking for confirmation where one is required (D8),
// downloading with progress and resume, and verifying checksums. Returns a
// process exit code.
int run_download(const std::vector<std::string>& args, const std::string& models_dir,
                 bool assume_yes);

void print_download_usage();

} // namespace pastiche
