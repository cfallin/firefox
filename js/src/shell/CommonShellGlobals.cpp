/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "shell/CommonShellGlobals.h"

#include <stdio.h>

#include "jsapi.h"  // JS_DefineFunction, JS_ValueToSource, JS_ClearPendingException

#include "js/CallArgs.h"
#include "js/CharacterEncoding.h"  // JS_EncodeStringToUTF8
#include "js/Conversions.h"        // JS::ToString
#include "js/Equality.h"           // JS::SameValue
#include "js/ErrorReport.h"        // JS_ReportErrorUTF8
#include "js/Printer.h"            // js::QuoteString
#include "js/RootingAPI.h"         // JS::Rooted
#include "js/Utility.h"            // JS::UniqueChars
#include "js/Value.h"

using namespace JS;

namespace {

// Best-effort source representation of a value for error messages.
const char* ValueToSource(JSContext* cx, HandleValue v, UniqueChars* bytes) {
  RootedString str(cx, JS_ValueToSource(cx, v));
  if (str) {
    *bytes = JS_EncodeStringToUTF8(cx, str);
    if (*bytes) {
      return bytes->get();
    }
  }
  JS_ClearPendingException(cx);
  return "<<error converting value to string>>";
}

bool Print(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return js::shell::PrintArgs(cx, args, stdout, /* newline = */ true);
}

}  // namespace

bool js::shell::PrintArgs(JSContext* cx, const CallArgs& args, FILE* out,
                          bool newline) {
  for (unsigned i = 0; i < args.length(); i++) {
    RootedString str(cx, ToString(cx, args[i]));
    if (!str) {
      return false;
    }
    UniqueChars bytes = JS_EncodeStringToUTF8(cx, str);
    if (!bytes) {
      return false;
    }
    fprintf(out, "%s%s", i ? " " : "", bytes.get());
  }
  if (newline) {
    fputc('\n', out);
  }
  fflush(out);
  args.rval().setUndefined();
  return true;
}

bool js::shell::AssertEq(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!(args.length() == 2 || (args.length() == 3 && args[2].isString()))) {
    JS_ReportErrorUTF8(cx, "assertEq: %s",
                       (args.length() < 2)    ? "not enough arguments"
                       : (args.length() == 3) ? "invalid arguments"
                                              : "too many arguments");
    return false;
  }

  bool same;
  if (!SameValue(cx, args[0], args[1], &same)) {
    return false;
  }
  if (!same) {
    UniqueChars bytes0, bytes1;
    const char* actual = ValueToSource(cx, args[0], &bytes0);
    const char* expected = ValueToSource(cx, args[1], &bytes1);
    if (args.length() == 2) {
      JS_ReportErrorUTF8(cx, "Assertion failed: got %s, expected %s", actual,
                         expected);
    } else {
      RootedString message(cx, args[2].toString());
      UniqueChars bytes2 = js::QuoteString(cx, message);
      if (!bytes2) {
        return false;
      }
      JS_ReportErrorUTF8(cx, "Assertion failed: got %s, expected %s: %s",
                         actual, expected, bytes2.get());
    }
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool js::shell::InstallCommonShellGlobals(JSContext* cx, HandleObject global) {
  return JS_DefineFunction(cx, global, "print", Print, 0, 0) &&
         JS_DefineFunction(cx, global, "assertEq", js::shell::AssertEq, 2, 0);
}
