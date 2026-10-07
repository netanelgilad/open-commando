#pragma once
#include <string>

// Map a Windows-style path ("Data\\Always.DAT") to the real POSIX path,
// matching existing components case-insensitively.
std::string Rebel_Resolve_Path(const char *path);
