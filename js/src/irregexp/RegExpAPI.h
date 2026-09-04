/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* This is the interface that the regexp engine exposes to SpiderMonkey. */

#ifndef regexp_RegExpAPI_h
#define regexp_RegExpAPI_h

#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Range.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"

#include "irregexp/RegExpTypes.h"
#include "js/ColumnNumber.h"  // JS::ColumnNumberOneOigin
#include "js/Stack.h"         // JS::NativeStackLimit
#include "vm/RegExpShared.h"

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSTracer;

namespace JS {
class RegExpFlags;
}

namespace v8::internal {
class RegExpStack;
}

namespace js {

class VectorMatchPairs;
class LifoAlloc;

namespace frontend {
class TokenStreamAnyChars;
}

#ifdef ENABLE_JS_NIGHTMONKEY
namespace night {
// AOT-compiled regex matchers (Wasm functions compiled alongside the
// scripts). Published by night_runtime_install_env; consulted by
// irregexp::Execute before falling into the bytecode interpreter. The matcher
// is called through a C function pointer whose value is a Wasm table index.
struct NightRegexEntry {
  const char16_t* pattern;
  uint32_t patternLen;
  uint32_t flags;
  // Wasm indirect-table indices of the per-encoding matchers; 0 = none.
  uint32_t latin1Idx;
  uint32_t twobyteIdx;
  uint32_t numRegisters;
  uint32_t pairCount;
};
// The published table, backtrack scratch, and diagnostics counters live on the
// runtime (js::night::NightRuntimeData, reached via cx->runtime()->nightData()):
// written once by night_runtime_install_env, consulted by irregexp::TryNightRegexMatch.
}  // namespace night
#endif

namespace irregexp {

#ifdef ENABLE_JS_NIGHTMONKEY
// Try the AOT-compiled Wasm matcher for this RegExpShared (single match).
// Returns true and sets *out when the matcher decided the match; false to
// fall back to the ordinary irregexp path. Called from RegExpShared::execute
// (the funnel for Matcher/Searcher/Tester/BuiltinExec), so the whole
// jit-choice/interpreter layering below is skipped on a hit.
bool TryNightRegexMatch(JSContext* cx, MutableHandleRegExpShared re,
                      Handle<JSLinearString*> input, size_t startIndex,
                      VectorMatchPairs* matches, bool latin1,
                      RegExpRunStatus* out);
#endif

Isolate* CreateIsolate(JSContext* cx);
void TraceIsolate(JSTracer* trc, Isolate* isolate);
void DestroyIsolate(Isolate* isolate);

size_t IsolateSizeOfIncludingThis(Isolate* isolate,
                                  mozilla::MallocSizeOf mallocSizeOf);

bool CheckPatternSyntax(
    js::LifoAlloc& alloc, JS::NativeStackLimit stackLimit,
    frontend::TokenStreamAnyChars& ts,
    const mozilla::Range<const char16_t> chars, JS::RegExpFlags flags,
    mozilla::Maybe<uint32_t> line = mozilla::Nothing(),
    mozilla::Maybe<JS::ColumnNumberOneOrigin> column = mozilla::Nothing());
bool CheckPatternSyntax(JSContext* cx, JS::NativeStackLimit stackLimit,
                        frontend::TokenStreamAnyChars& ts,
                        Handle<JSAtom*> pattern, JS::RegExpFlags flags);

bool CompilePattern(JSContext* cx, MutableHandleRegExpShared re,
                    Handle<JSLinearString*> input,
                    RegExpShared::CodeKind codeKind);

RegExpRunStatus Execute(JSContext* cx, MutableHandleRegExpShared re,
                        Handle<JSLinearString*> input, size_t start,
                        VectorMatchPairs* matches);

RegExpRunStatus ExecuteForFuzzing(JSContext* cx, Handle<JSAtom*> pattern,
                                  Handle<JSLinearString*> input,
                                  JS::RegExpFlags flags, size_t startIndex,
                                  VectorMatchPairs* matches,
                                  RegExpShared::CodeKind codeKind);

bool GrowBacktrackStack(v8::internal::RegExpStack* regexp_stack);

uint32_t CaseInsensitiveCompareNonUnicode(const char16_t* substring1,
                                          const char16_t* substring2,
                                          size_t byteLength);
uint32_t CaseInsensitiveCompareUnicode(const char16_t* substring1,
                                       const char16_t* substring2,
                                       size_t byteLength);
bool IsCharacterInRangeArray(uint32_t c, ByteArrayData* ranges);

#ifdef DEBUG
bool IsolateShouldSimulateInterrupt(Isolate* isolate);
void IsolateSetShouldSimulateInterrupt(Isolate* isolate);
void IsolateClearShouldSimulateInterrupt(Isolate* isolate);
#endif
}  // namespace irregexp
}  // namespace js

#endif /* regexp_RegExpAPI_h */
