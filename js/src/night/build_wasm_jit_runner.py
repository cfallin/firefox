# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Build the test-only wasm-jit-runner with its own cargo project and
# install it into dist/bin. Invoked as a forced GENERATED_FILES script
# (see moz.build): cargo owns the incrementality. (nightmonkey gets the
# same treatment in build_nightmonkey.py, but is not test-only.)

import os
import shutil
import subprocess

import buildconfig

PROJECTS = [
    ("wasm-jit-runner", "wasm-jit-runner"),
]


def main(output):
    srcdir = os.path.dirname(os.path.abspath(__file__))
    cargo = buildconfig.substs.get("CARGO") or "cargo"
    dist_bin = os.path.join(buildconfig.topobjdir, "dist", "bin")
    os.makedirs(dist_bin, exist_ok=True)

    # The enclosing build targets wasm32 and exports target-oriented
    # RUSTFLAGS/target selection; these are HOST tools built with each
    # project's own profile, so scrub those knobs and keep the in-crate
    # target/ directories (shared with manual `cargo build` invocations).
    env = dict(os.environ)
    for var in ("RUSTFLAGS", "CARGO_BUILD_TARGET", "CARGO_TARGET_DIR"):
        env.pop(var, None)

    installed = []
    for project, binary in PROJECTS:
        projdir = os.path.join(srcdir, project)
        subprocess.check_call([cargo, "build", "--release"], cwd=projdir, env=env)
        src = os.path.join(projdir, "target", "release", binary)
        dst = os.path.join(dist_bin, binary)
        # Copy via a temp name + rename so a concurrently running binary is
        # never truncated in place.
        tmp = dst + ".tmp"
        shutil.copy2(src, tmp)
        os.replace(tmp, dst)
        installed.append(dst)

    output.write("".join(p + "\n" for p in installed))
