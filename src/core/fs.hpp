#pragma once

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

// Small filesystem layer. All paths are UTF-8; on Windows they are converted to
// UTF-16 before touching the OS so non-ASCII names work with MinGW.
namespace pastiche {

FILE* fopen_utf8(const std::string& path, const char* mode);

// Returns "" on success, error message otherwise.
std::string read_file(const std::string& path, std::vector<uint8_t>& out);
std::string write_file(const std::string& path, const void* data, size_t size);
std::string write_text_file(const std::string& path, const std::string& text);

bool file_exists(const std::string& path);
bool dir_exists(const std::string& path);
bool make_dirs(const std::string& path);                 // mkdir -p; true when it exists afterwards
std::vector<std::string> list_dir(const std::string& path);  // file names only, sorted
uint64_t file_size(const std::string& path);              // 0 when missing

std::string path_join(const std::string& a, const std::string& b);
std::string path_dirname(const std::string& p);           // "" when no directory part
std::string path_basename(const std::string& p);
std::string path_stem(const std::string& p);              // basename without extension
std::string path_extension(const std::string& p);         // lower-case, with dot, "" when none
std::string replace_extension(const std::string& p, const std::string& new_ext);
std::string absolute_path(const std::string& p);

std::string exe_dir();                                    // directory of the running executable
std::string timestamp_now();                              // "YYYYMMDD_HHMMSS", local time
std::string iso_time_now();                               // "YYYY-MM-DDTHH:MM:SS", local time
std::string getenv_utf8(const char* name);                // "" when unset

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);
// Command line as UTF-8 (from GetCommandLineW), including argv[0].
std::vector<std::string> utf8_argv();
// Switch console output to UTF-8 so messages with non-ASCII paths print correctly.
void setup_console_utf8();
#endif

} // namespace pastiche
