#pragma once

namespace datadam {

// The commit this binary was built from (40 hex digits), or "unknown". The
// version is DATADAM_VERSION, the project's version in CMakeLists.txt.
const char* git_commit();

}  // namespace datadam
