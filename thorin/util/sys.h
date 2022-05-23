#ifndef THORIN_UTIL_SYS_H
#define THORIN_UTIL_SYS_H

#ifdef _WIN32
#    define THORIN_WHICH "where"
#else
#    define THORIN_WHICH "which"
#endif

#include <filesystem>
#include <optional>
#include <string>

namespace thorin::sys {

/// @returns `std::nullopt` if an error occurred.
std::optional<std::filesystem::path> path_to_curr_exe();

/// Executes command @p cmd. @returns the output as string.
std::string exec(std::string& cmd);

/// Wraps `std::system` and makes the return value usable.
int system(const std::string&);

/// Wraps sys::system and puts `.exe` at the back (Windows) and `./` at the front (otherwise) of @p cmd.
int run(std::string cmd, const std::string& args = {});

std::string find_cmd(const std::string&);

} // namespace thorin::sys

#endif
