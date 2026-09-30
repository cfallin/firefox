/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Hooks for an external compilation tier layered on top of the engine.
 *
 * Built only with --enable-external-compiler-hooks
 * (JS_EXTERNAL_COMPILER_HOOKS). The engine reserves one pointer-sized word per
 * object, per script and per RegExpShared for the external tier's use, calls
 * the hook table registered on the runtime at the points where the tier's
 * assumptions about the heap or control flow can be invalidated, and lets the
 * tier take over script entry. Each context caches the runtime's table and
 * carries an opaque per-context state the tier creates and destroys with the
 * context. With no table registered every hook site costs one load and a
 * predictable branch.
 */

#ifndef js_ExternalCompilerHooks_h
#define js_ExternalCompilerHooks_h

#ifdef JS_EXTERNAL_COMPILER_HOOKS

#  include <stddef.h>
#  include <stdint.h>

#  include "jstypes.h"

#  include "js/CallArgs.h"
#  include "js/Id.h"
#  include "js/RootingAPI.h"
#  include "js/TypeDecls.h"
#  include "js/Value.h"

namespace js {
class AbstractGeneratorObject;
class NativeObject;
class SharedShape;
class RegExpShared;
class RunState;
class VectorMatchPairs;
enum class RegExpRunStatus : int32_t;
}  // namespace js

namespace JS {

// Why an object's external word was reset.
enum class ExternalObjectMutation : uint32_t {
  ToDictionary = 1,
  ChangeProperty,
  ChangeCustomDataProp,
  RemoveProperty,
  FreezeOrSeal,
  Swap,
  ObjectFlagChange,
  // A slot store through an engine path cleared bits per the store masks.
  StoredValue,
};

enum class ExternalEnterStatus : uint32_t { NotEntered, Ok, Error };

// Every hook receives the context the engine is running on. The one site
// without a context of its own, the slot-store choke, passes the runtime's
// main context.
struct ExternalCompilerHooks {
  // Per-context state: created when a context is initialized (or when the
  // table is registered on a runtime whose context already exists) and
  // destroyed with the context. JSContext::getExternalCompilerState returns
  // it. Either may be null.
  void* (*newContext)(JSContext* cx);
  void (*destroyContext)(JSContext* cx, void* state);

  // Object model. Every object carries a pointer-sized external word, zero
  // at birth. A structural change (shape change, dictionary mode, freeze,
  // flag change, swap) on an object whose word is nonzero resets the word to
  // zero and reports the old word here.
  void (*objectDemoted)(JSContext* cx, JSObject* obj, uintptr_t oldWord,
                        ExternalObjectMutation why);
  // Slot-store policy, applied on every engine-path slot store to an object
  // whose word intersects either mask: storeClearMask bits are cleared on any
  // store, storeNonNumberClearMask bits when the value is not a number. When
  // the word changes, objectDemoted fires with StoredValue.
  uintptr_t storeClearMask;
  uintptr_t storeNonNumberClearMask;
  // A property was added to an object whose word is nonzero.
  void (*propertyAdded)(JSContext* cx, js::NativeObject* obj,
                        JS::PropertyKey id, uint32_t slot,
                        uint32_t numFixedSlots);
  // Slot placement. Before the engine adds a property to a shared-shape
  // object whose word is nonzero, it asks here where to put it: set *result
  // to a shape from js::ExternalShapeWithPropertyAtSlot, made from the
  // object's current shape with this key and flags (the raw PropertyFlags
  // byte), to place the property in that shape's slot, or leave it null to
  // let the engine append the property at the slot span. Return false only
  // with an exception pending. Engine-side caches keyed by (shape, key) are
  // not filled with such shapes; the tier memoizes its own.
  bool (*shapeForAdd)(JSContext* cx, JS::Handle<js::NativeObject*> obj,
                      JS::HandleId id, uint8_t flags,
                      js::SharedShape** result);

  // Global object. A property was defined or deleted on a global, a data
  // property of a global was written, or a global lexical binding now
  // shadows a same-named global property.
  void (*globalKeyChanged)(JSContext* cx, JS::PropertyKey id);
  void (*globalDataStored)(JSContext* cx, JS::PropertyKey id,
                           uint64_t valueBits);
  void (*globalLexicalShadowAdded)(JSContext* cx, uint64_t idBits);

  // Script entry. Every script carries a pointer-sized external word, zero
  // at birth; the engine consults these only for scripts whose word is
  // nonzero.
  ExternalEnterStatus (*enterScript)(JSContext* cx, js::RunState& state);
  ExternalEnterStatus (*enterCall)(JSContext* cx, const JS::CallArgs& args,
                                   JSScript* script, bool constructing);
  // A generator whose frame belongs to the external tier cannot be resumed
  // by the interpreter.
  bool (*isForeignGenerator)(JSContext* cx, js::AbstractGeneratorObject* gen);
  ExternalEnterStatus (*resumeGenerator)(
      JSContext* cx, JS::Handle<js::AbstractGeneratorObject*> gen,
      JS::HandleValue arg, JS::HandleValue resumeKind,
      JS::MutableHandleValue rval);

  // GC: traced on every collection, minor and major.
  void (*traceRoots)(JSContext* cx, JSTracer* trc);

  // Frontend: source text entered the compiler (eval, Function, module or
  // embedder compile alike). Not reported for a compile with no context of
  // its own (an off-thread compile).
  void (*sourceAssigned)(JSContext* cx);
  // Lower bound on the fixed-slot estimate for constructor `this` objects
  // (0 leaves the engine's estimate alone).
  uint32_t minConstructorThisSlots;

  // RegExp: decide a match before the engine's own matcher runs. Return true
  // with *status set to take over; false to fall through.
  bool (*regexpMatch)(JSContext* cx, JS::MutableHandle<js::RegExpShared*> re,
                      JS::Handle<JSLinearString*> input, size_t start,
                      js::VectorMatchPairs* matches, bool latin1,
                      js::RegExpRunStatus* status);
};

// Register (or, with nullptr, unregister) the hook table on a runtime. The
// table must outlive its registration. The runtime's context caches the
// table; per-context state is created for it here (and destroyed on
// unregistration). Call from the runtime's own thread, before any script the
// tier cares about runs.
extern JS_PUBLIC_API void SetExternalCompilerHooks(
    JSRuntime* rt, ExternalCompilerHooks* hooks);
extern JS_PUBLIC_API ExternalCompilerHooks* GetExternalCompilerHooks(
    JSRuntime* rt);

}  // namespace JS

namespace js {

// Out-of-line halves of the inline hook sites (vm/JSObject.h).
extern JS_PUBLIC_API void ExternalObjectDemoted(JSContext* cx, JSObject* obj,
                                                uintptr_t oldWord,
                                                JS::ExternalObjectMutation why);
extern JS_PUBLIC_API void ExternalObjectStore(JSObject* obj,
                                              const JS::Value& v);
extern JS_PUBLIC_API void ExternalPropertyAdded(JSContext* cx,
                                                NativeObject* obj,
                                                JS::PropertyKey id,
                                                uint32_t slot);
extern JS_PUBLIC_API bool ExternalShapeForAdd(JSContext* cx,
                                              JS::Handle<NativeObject*> obj,
                                              JS::HandleId id, uint8_t flags,
                                              SharedShape** result);

// Custom slot placement (SharedShape::getShapeWithPropertyAtSlot): the shape
// that adds `id` with raw PropertyFlags `flags` to `shape` in slot `slot`, or
// null in *result (no exception) if that placement is not possible. A slot
// other than the span gives the shape ObjectFlag::PermutedSlots, which turns
// off the engine's fast paths that assume slot order is insertion order.
extern JS_PUBLIC_API bool ExternalShapeWithPropertyAtSlot(
    JSContext* cx, JS::Handle<SharedShape*> shape, JS::HandleId id,
    uint8_t flags, uint32_t slot, SharedShape** result);

// Add the property `newShape` adds to `obj`, whose shape it was made from
// (see NativeObject::addPropertyWithShape). The slot, holding undefined, is
// returned in *slot for the caller to initialize.
extern JS_PUBLIC_API bool ExternalAddPropertyWithShape(
    JSContext* cx, JS::Handle<NativeObject*> obj, SharedShape* newShape,
    uint32_t* slot);

}  // namespace js

#endif  // JS_EXTERNAL_COMPILER_HOOKS

#endif  // js_ExternalCompilerHooks_h
