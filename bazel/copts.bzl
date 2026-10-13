"""Common copts for building wolaz.

Warnings are applied per-target rather than through .bazelrc so that they
affect wolaz's own code only. Setting them globally would turn every warning
in protobuf and abseil into build noise, and would mean a dependency's
warnings could not be distinguished from ours.
"""

visibility([
    "//bazel/...",
    "//conformance/...",
    "//plugin/...",
    "//runtime/...",
    "//test/...",
])

WOLAZ_COPTS = [
    "-Wall",
    "-Wextra",
]

# The runtime and the code generated against it are C++14. .bazelrc sets
# C++17 for everything, because protobuf and abseil require it; these flags
# come later on the command line, so they win for the targets that use them.
#
# -pedantic-errors matters: without it GCC accepts many C++17 features in
# C++14 mode with only a warning, so C++17-isms would slip through.
WOLAZ_CPP14_COPTS = WOLAZ_COPTS + [
    "-std=c++14",
    "-pedantic-errors",
]

# The plugin links libprotoc, whose headers emit warnings we do not control.
WOLAZ_PLUGIN_COPTS = [
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
]
