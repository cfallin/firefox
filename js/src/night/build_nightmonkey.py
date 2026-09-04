# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Build the `nightmonkey` AOT snapshot transform with its own cargo project
# and install it into dist/host/bin. Invoked as a forced GENERATED_FILES
# script (see moz.build): cargo owns the incrementality.

import os
import shutil
import subprocess

import buildconfig


def main(output):
    srcdir = os.path.dirname(os.path.abspath(__file__))
    cargo = buildconfig.substs.get("CARGO") or "cargo"
    dist_host_bin = os.path.join(buildconfig.topobjdir, "dist", "host", "bin")
    os.makedirs(dist_host_bin, exist_ok=True)

    # The enclosing build targets wasm32 and exports target-oriented
    # RUSTFLAGS/target selection; this is a HOST tool built with the
    # project's own profile, so scrub those knobs and keep the in-crate
    # target/ directory (shared with manual `cargo build` invocations).
    env = dict(os.environ)
    for var in ("RUSTFLAGS", "CARGO_BUILD_TARGET", "CARGO_TARGET_DIR"):
        env.pop(var, None)

    projdir = os.path.join(srcdir, "nightmonkey")
    subprocess.check_call(
        [cargo, "build", "--release", "--features", "wizen"],
        cwd=projdir,
        env=env,
    )
    src = os.path.join(projdir, "target", "release", "nightmonkey")
    dst = os.path.join(dist_host_bin, "nightmonkey")
    # Copy via a temp name + rename so a concurrently running binary is
    # never truncated in place.
    tmp = dst + ".tmp"
    shutil.copy2(src, tmp)
    os.replace(tmp, dst)

    output.write(dst + "\n")
