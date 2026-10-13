"""Public rule for generating wolaz code from protos:
  - wolaz_proto_library()
"""

load("@com_google_protobuf//bazel/common:proto_info.bzl", "ProtoInfo")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load(
    "//bazel/private:wolaz_proto_library_internal.bzl",
    _WolazGeneratedSrcsInfo = "WolazGeneratedSrcsInfo",
    _aspect = "wolaz_proto_library_aspect",
    _impl = "wolaz_proto_library_impl",
)

WolazGeneratedSrcsInfo = _WolazGeneratedSrcsInfo

wolaz_proto_library = rule(
    implementation = _impl,
    doc = """Generates wolaz C++ headers for a proto_library.

Returns a CcInfo, so an ordinary cc_library or cc_binary can depend on it
directly and get both the generated headers and the wolaz runtime:

    proto_library(
        name = "msample_proto",
        srcs = ["msample.proto"],
    )

    wolaz_proto_library(
        name = "msample_wolaz_proto",
        deps = [":msample_proto"],
    )

    cc_binary(
        name = "app",
        srcs = ["app.cc"],
        deps = [":msample_wolaz_proto"],
    )

Headers are generated for the whole transitive closure of `deps`, because a
generated header includes the generated headers of its imports.
""",
    attrs = {
        "deps": attr.label_list(
            aspects = [_aspect],
            providers = [ProtoInfo],
            doc = "Exactly one proto_library to generate code for.",
        ),
    },
    provides = [CcInfo],
)
