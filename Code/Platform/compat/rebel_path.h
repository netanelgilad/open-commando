#pragma once
#include <string>

// Map a Windows-style path ("Data\\Always.DAT") to the real POSIX path,
// matching existing components case-insensitively.
std::string Rebel_Resolve_Path(const char *path);

// Same, and reports whether the path is known not to exist: a component was
// missing from its (cached) directory listing. Lets lookups of absent files
// fail without a round trip to the OPFS thread.
std::string Rebel_Resolve_Path(const char *path, bool *known_missing);

// Pooled read-only file descriptors (win32_file.cpp) for a resolved path are
// closed: the file is about to be written, replaced or removed.
void Rebel_File_Pool_Forget(const std::string &resolved_path);
