/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Common shell-style global builtins, factored out of the js shell so that
// minimal embeddings (currently the AOT runtime reactor, js/src/night/runtime/) can
// install the same `print`/`assertEq` the shell exposes without depending on
// the shell program itself. Homed under shell/ so the dependency runs
// embedding -> shell, never the reverse. Self-contained: uses only the public
// JS API (plus js::QuoteString), no shell-only state.

#ifndef shell_CommonShellGlobals_h
#define shell_CommonShellGlobals_h

#include <stdio.h>

#include "js/CallArgs.h"
#include "js/TypeDecls.h"

namespace js {
namespace shell {

// The shared `print` loop: ToString each argument and write it to `out`,
// space-separated, with a trailing newline when `newline` is set. Sets
// args.rval() to undefined. The shell's redirectable print/printErr wrap this
// (passing their RCFile's stream); InstallCommonShellGlobals uses it for
// stdout.
[[nodiscard]] bool PrintArgs(JSContext* cx, const JS::CallArgs& args, FILE* out,
                             bool newline);

// assertEq(actual, expected[, message]): throw if SameValue(actual, expected)
// is false. Error messages match the shell's historical text.
bool AssertEq(JSContext* cx, unsigned argc, JS::Value* vp);

// Install the modeled common globals on `global`: `print` (-> stdout) and
// `assertEq`. For minimal embeddings; the shell defines its own redirectable
// print but shares PrintArgs/AssertEq above.
[[nodiscard]] bool InstallCommonShellGlobals(JSContext* cx,
                                             JS::HandleObject global);

}  // namespace shell
}  // namespace js

#endif  // shell_CommonShellGlobals_h
