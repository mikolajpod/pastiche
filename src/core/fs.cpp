#include "fs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace pastiche {

namespace {

fs::path to_path(const std::string& utf8)
{
#ifdef _WIN32
    return fs::path(utf8_to_wide(utf8));
#else
    return fs::path(utf8);
#endif
}

std::string from_path(const fs::path& p)
{
#ifdef _WIN32
    return wide_to_utf8(p.wstring());
#else
    return p.string();
#endif
}

} // namespace

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wide_to_utf8(const std::wstring& s)
{
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::vector<std::string> utf8_argv()
{
    std::vector<std::string> out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return out;
    for (int i = 0; i < argc; ++i) out.push_back(wide_to_utf8(argv[i]));
    LocalFree(argv);
    return out;
}

void setup_console_utf8()
{
    SetConsoleOutputCP(CP_UTF8);
}
#endif

FILE* fopen_utf8(const std::string& path, const char* mode)
{
#ifdef _WIN32
    std::wstring wmode = utf8_to_wide(mode);
    return _wfopen(utf8_to_wide(path).c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

std::string read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = fopen_utf8(path, "rb");
    if (!f) return "cannot open '" + path + "' for reading";
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    if (len < 0) { std::fclose(f); return "cannot determine size of '" + path + "'"; }
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(len));
    const size_t got = len > 0 ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    if (got != out.size()) return "short read on '" + path + "'";
    return {};
}

std::string write_file(const std::string& path, const void* data, size_t size)
{
    FILE* f = fopen_utf8(path, "wb");
    if (!f) return "cannot open '" + path + "' for writing";
    const size_t put = size ? std::fwrite(data, 1, size, f) : 0;
    const bool closed = std::fclose(f) == 0;
    if (put != size || !closed) {
        std::error_code ec;
        fs::remove(to_path(path), ec);
        return "write error on '" + path + "' (disk full?)";
    }
    return {};
}

std::string write_text_file(const std::string& path, const std::string& text)
{
    return write_file(path, text.data(), text.size());
}

bool file_exists(const std::string& path)
{
    std::error_code ec;
    return fs::is_regular_file(to_path(path), ec);
}

bool dir_exists(const std::string& path)
{
    std::error_code ec;
    return fs::is_directory(to_path(path), ec);
}

bool make_dirs(const std::string& path)
{
    if (path.empty()) return true;
    std::error_code ec;
    fs::create_directories(to_path(path), ec);
    return dir_exists(path);
}

std::vector<std::string> list_dir(const std::string& path)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(to_path(path), ec))
        out.push_back(from_path(e.path().filename()));
    std::sort(out.begin(), out.end());
    return out;
}

uint64_t file_size(const std::string& path)
{
    std::error_code ec;
    const auto s = fs::file_size(to_path(path), ec);
    return ec ? 0 : static_cast<uint64_t>(s);
}

std::string path_join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a.back();
    if (last == '/' || last == '\\') return a + b;
#ifdef _WIN32
    return a + "\\" + b;   // native separator, so paths shown to the user are not mixed
#else
    return a + "/" + b;
#endif
}

std::string path_dirname(const std::string& p)
{
    const size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return {};
    if (pos == 0) return p.substr(0, 1);
    return p.substr(0, pos);
}

std::string path_basename(const std::string& p)
{
    const size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string path_stem(const std::string& p)
{
    const std::string b = path_basename(p);
    const size_t dot = b.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return b;
    return b.substr(0, dot);
}

std::string path_extension(const std::string& p)
{
    const std::string b = path_basename(p);
    const size_t dot = b.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return {};
    std::string ext = b.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::string replace_extension(const std::string& p, const std::string& new_ext)
{
    const std::string ext = path_extension(p);
    const std::string base = ext.empty() ? p : p.substr(0, p.size() - ext.size());
    return base + new_ext;
}

std::string absolute_path(const std::string& p)
{
    std::error_code ec;
    fs::path a = fs::absolute(to_path(p), ec);
    if (ec) return p;
    return from_path(a.lexically_normal());
}

std::string exe_dir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return ".";
    return path_dirname(wide_to_utf8(std::wstring(buf, n)));
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    return path_dirname(buf);
#endif
}

std::string timestamp_now()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

std::string iso_time_now()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::string getenv_utf8(const char* name)
{
#ifdef _WIN32
    const std::wstring wname = utf8_to_wide(name);
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring w(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(wname.c_str(), w.data(), n);
    w.resize(got);
    return wide_to_utf8(w);
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

} // namespace pastiche
