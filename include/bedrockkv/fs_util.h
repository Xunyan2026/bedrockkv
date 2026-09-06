// BedrockKV — small durability helpers for files and directories.
//
// The crash-consistency story of the storage engine rests on a strict
// rule: a file's data AND its directory entry must be on stable storage
// before anything that references it becomes durable (the MANIFEST, in
// our case). fsync(file) alone is not enough — the rename that publishes
// the file can still be sitting in the directory inode's dirty state
// after a power cut. Hence SyncDir after every creation/rename.
#pragma once

#include <string>
#include <string_view>

#include "bedrockkv/status.h"

namespace bedrockkv::fs {

// Flushes a directory's inode to stable storage. No-op is not allowed:
// on failure returns an error — callers treat it as fatal.
Status SyncDir(const std::string& dir);

// Writes `data` to `path` (O_WRONLY|O_CREAT|O_TRUNC), fsyncs the file,
// then fsyncs the parent directory so the file's existence is durable.
Status WriteFileDurable(const std::string& path, std::string_view data);

// Reads a whole file. kIOError if the file cannot be opened/read.
Status ReadFileToString(const std::string& path, std::string* out);

}  // namespace bedrockkv::fs
