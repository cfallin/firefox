/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Support for Wizer-based snapshotting of the JS shell, when built
 * for a Wasm target (i.e., running inside a Wasm module). */

#include "jsfriendapi.h"  // js::RunJobs

#include "js/CallAndConstruct.h"  // JS_CallFunctionName
#include "shell/jsshell.h"
#ifdef ENABLE_JS_NIGHTMONKEY
#  include "night/runtime/NightRegistration.h"
#endif

using namespace js;
using namespace js::shell;

#ifdef JS_SHELL_WIZER

#  include <wizer.h>

static std::optional<JSAndShellContext> wizenedContext;

static void WizerInit() {
  // Wizening a NightMonkey shell exists only to produce an AOT snapshot, so
  // the snapshot root registration is always on: the top level runs here,
  // during wizening, and the resumed snapshot calls the program's main().
#  ifdef ENABLE_JS_NIGHTMONKEY
  const int argc = 2;
  char* argv[3] = {strdup("js"), strdup("--night-snapshot"), NULL};
#  else
  const int argc = 1;
  char* argv[2] = {strdup("js"), NULL};
#  endif

  auto ret = ShellMain(argc, argv, /* retainContext = */ true);
  if (!ret.is<JSAndShellContext>()) {
    fprintf(stderr, "Could not execute shell main during Wizening!\n");
    abort();
  }

  wizenedContext = std::move(ret.as<JSAndShellContext>());
}

WIZER_INIT(WizerInit);

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  if (wizenedContext) {
    JSContext* cx = wizenedContext.value().cx;
    RootedObject glob(cx, wizenedContext.value().glob);

    JSAutoRealm ar(cx, glob);

#  ifdef ENABLE_JS_NIGHTMONKEY
    JS::NightActivate(cx);
#  endif

    // Look up a function called "main" in the global.
    JS::Rooted<JS::Value> ret(cx);
    // `glob`, not `cx->global()`: the latter is a Handle<GlobalObject*>,
    // which does not convert to the Handle<JSObject*> this takes.
    if (!JS_CallFunctionName(cx, glob, "main", JS::HandleValueArray::empty(),
                             &ret)) {
      fprintf(stderr, "Failed to call main() in Wizened JS source!\n");
      abort();
    }
    // Drain the microtask queue, as the shell's own run loop does after
    // every script: a program whose main() leaves promise continuations
    // queued (any `async` function, any `.then`) would otherwise exit with
    // them unrun -- the work is complete but its result never observed.
    js::RunJobs(cx);
  } else {
    return ShellMain(argc, argv, /* returnContext = */ false).as<int>();
  }
}

#endif  // JS_SHELL_WIZER
