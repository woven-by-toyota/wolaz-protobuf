"""Implementation of the aspect that powers wolaz_proto_library().

Modelled on protobuf's own upb_proto_library aspect
(upb/bazel/private/upb_proto_library_internal/aspect.bzl).

An aspect is required rather than a plain rule because generated headers
`#include` the generated headers of their imports: `test_messages_proto3.proto`
imports `google/protobuf/any.proto`, so the header for the former contains
`#include "google/protobuf/any.wolaz.hpp"`. Running protoc over only the
directly requested .proto file would leave those includes dangling. The aspect
walks `deps` and generates for every proto_library in the transitive closure.

Unlike upb's, this aspect compiles nothing. protoc-gen-wolaz emits headers
only, so the generated code needs no library of its own -- it just needs to end
up in the compilation context of whatever depends on it, together with the
runtime.
"""

load("@com_google_protobuf//bazel/common:proto_common.bzl", "proto_common")
load("@com_google_protobuf//bazel/common:proto_info.bzl", "ProtoInfo")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

WolazGeneratedSrcsInfo = provider(
    doc = "The .wolaz.hpp files generated for one proto_library.",
    fields = {
        "hdrs": "(list[File]) generated headers, direct sources only",
        "transitive_hdrs": "(depset[File]) generated headers, including imports",
    },
)

WolazWrappedCcInfo = provider(
    doc = "CcInfo for generated wolaz headers, propagated along the aspect.",
    fields = {
        "cc_info": "(CcInfo) headers and include paths for the generated code",
    },
)

def _output_dir(ctx, proto_info):
    """The -I path under which the generated headers appear.

    Generated files land beside their .proto under the bin directory, rooted at
    the proto_source_root. A `#include "google/protobuf/any.wolaz.hpp"` thus
    resolves only if that root is on the include path.
    """
    proto_root = proto_info.proto_source_root
    if proto_root == ".":
        return ctx.bin_dir.path
    if proto_root.startswith(ctx.bin_dir.path):
        return proto_root
    return ctx.bin_dir.path + "/" + proto_root

def _wolaz_proto_aspect_impl(target, ctx):
    proto_info = target[ProtoInfo]
    toolchain = ctx.attr._wolaz_toolchain[proto_common.ProtoLangToolchainInfo]

    hdrs = proto_common.declare_generated_files(
        actions = ctx.actions,
        proto_info = proto_info,
        extension = ".wolaz.hpp",
    )

    # A proto_library may be a pure forwarding target with no srcs of its own,
    # in which case there is nothing to run protoc on.
    if hdrs:
        proto_common.compile(
            actions = ctx.actions,
            proto_info = proto_info,
            proto_lang_toolchain_info = toolchain,
            generated_files = hdrs,
        )

    deps = getattr(ctx.rule.attr, "deps", [])
    dep_cc_infos = [d[WolazWrappedCcInfo].cc_info for d in deps if WolazWrappedCcInfo in d]
    dep_hdrs = [d[WolazGeneratedSrcsInfo].transitive_hdrs for d in deps if WolazGeneratedSrcsInfo in d]

    # The runtime that generated code includes, declared once on the toolchain
    # rather than repeated at every use site.
    runtime_cc_infos = []
    if toolchain.runtime and CcInfo in toolchain.runtime:
        runtime_cc_infos.append(toolchain.runtime[CcInfo])

    own_cc_info = CcInfo(
        compilation_context = cc_common.create_compilation_context(
            headers = depset(hdrs),
            includes = depset([_output_dir(ctx, proto_info)]),
        ),
    )

    return [
        WolazGeneratedSrcsInfo(
            hdrs = hdrs,
            transitive_hdrs = depset(hdrs, transitive = dep_hdrs),
        ),
        WolazWrappedCcInfo(
            cc_info = cc_common.merge_cc_infos(
                direct_cc_infos = [own_cc_info],
                cc_infos = dep_cc_infos + runtime_cc_infos,
            ),
        ),
    ]

wolaz_proto_library_aspect = aspect(
    implementation = _wolaz_proto_aspect_impl,
    attr_aspects = ["deps"],
    attrs = {
        "_wolaz_toolchain": attr.label(
            default = Label("//plugin:wolaz_toolchain"),
        ),
    },
    required_providers = [ProtoInfo],
    provides = [WolazGeneratedSrcsInfo, WolazWrappedCcInfo],
)

def wolaz_proto_library_impl(ctx):
    """Shared implementation for the wolaz_proto_library rule."""
    if len(ctx.attr.deps) != 1:
        fail("wolaz_proto_library expects exactly one proto_library in deps")

    dep = ctx.attr.deps[0]
    srcs = dep[WolazGeneratedSrcsInfo]
    return [
        DefaultInfo(files = srcs.transitive_hdrs),
        srcs,
        dep[WolazWrappedCcInfo].cc_info,
    ]
