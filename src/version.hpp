// The version the binary was built from, for the first line of every log.
#pragma once

namespace nmm {
// "commit <hash> <date> <subject> (branch <branch>[, + local changes]), code <fingerprint>"
const char* build_version();
}  // namespace nmm
