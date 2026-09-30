/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/ExternalCompilerHooks.h"

#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/Runtime.h"
#include "vm/Shape.h"

using namespace js;

void JSRuntime::setExternalCompilerHooks(JS::ExternalCompilerHooks* hooks) {
  externalCompilerHooks_ = hooks;
  externalObjectStoreMask_ =
      hooks ? (hooks->storeClearMask | hooks->storeNonNumberClearMask) : 0;
}

JS_PUBLIC_API void JS::SetExternalCompilerHooks(JSRuntime* rt,
                                                ExternalCompilerHooks* hooks) {
  rt->setExternalCompilerHooks(hooks);
  if (JSContext* cx = rt->mainContextFromAnyThread()) {
    cx->updateExternalCompilerHooks();
  }
}

JS_PUBLIC_API JS::ExternalCompilerHooks* JS::GetExternalCompilerHooks(
    JSRuntime* rt) {
  return rt->externalCompilerHooks();
}

void JSContext::updateExternalCompilerHooks() {
  destroyExternalCompilerState();
  externalCompilerHooks_ = runtime()->externalCompilerHooks();
  if (externalCompilerHooks_ && externalCompilerHooks_->newContext) {
    externalCompilerState_ = externalCompilerHooks_->newContext(this);
  }
}

void JSContext::destroyExternalCompilerState() {
  if (externalCompilerState_ && externalCompilerHooks_ &&
      externalCompilerHooks_->destroyContext) {
    externalCompilerHooks_->destroyContext(this, externalCompilerState_);
  }
  externalCompilerState_ = nullptr;
}

JS_PUBLIC_API void js::ExternalObjectDemoted(JSContext* cx, JSObject* obj,
                                             uintptr_t oldWord,
                                             JS::ExternalObjectMutation why) {
  JS::ExternalCompilerHooks* hooks = cx->externalCompilerHooks();
  if (hooks && hooks->objectDemoted) {
    hooks->objectDemoted(cx, obj, oldWord, why);
  }
}

JS_PUBLIC_API void js::ExternalObjectStore(JSObject* obj, const JS::Value& v) {
  // The store chokes carry no context: reach the table through the object's
  // runtime, and report on the runtime's main context.
  JSRuntime* rt = obj->runtimeFromAnyThread();
  JS::ExternalCompilerHooks* hooks = rt->externalCompilerHooks();
  if (!hooks) {
    return;
  }
  uintptr_t w = obj->externalWord();
  if ((w & rt->externalObjectStoreMask()) == 0) {
    return;
  }
  uintptr_t nw = w & ~hooks->storeClearMask;
  if (!v.isNumber()) {
    nw &= ~hooks->storeNonNumberClearMask;
  }
  if (nw != w) {
    obj->setExternalWord(nw);
    if (hooks->objectDemoted) {
      hooks->objectDemoted(rt->mainContextFromAnyThread(), obj, w,
                           JS::ExternalObjectMutation::StoredValue);
    }
  }
}

JS_PUBLIC_API void js::ExternalPropertyAdded(JSContext* cx, NativeObject* obj,
                                             JS::PropertyKey id,
                                             uint32_t slot) {
  JS::ExternalCompilerHooks* hooks = cx->externalCompilerHooks();
  if (hooks && hooks->propertyAdded) {
    hooks->propertyAdded(cx, obj, id, slot, obj->numFixedSlots());
  }
}

JS_PUBLIC_API bool js::ExternalShapeForAdd(JSContext* cx,
                                           JS::Handle<NativeObject*> obj,
                                           JS::HandleId id, uint8_t flags,
                                           SharedShape** result) {
  *result = nullptr;
  JS::ExternalCompilerHooks* hooks = cx->externalCompilerHooks();
  if (!hooks || !hooks->shapeForAdd) {
    return true;
  }
  if (!hooks->shapeForAdd(cx, obj, id, flags, result)) {
    return false;
  }
  if (SharedShape* shape = *result) {
    // Cheap validation of the tier's answer; addPropertyWithShape checks
    // that the shape extends the object's by one property.
    PropertyInfoWithKey prop = shape->lastProperty();
    MOZ_RELEASE_ASSERT(prop.key() == id.get());
    MOZ_RELEASE_ASSERT(prop.flags().toRaw() == flags);
  }
  return true;
}

JS_PUBLIC_API bool js::ExternalShapeWithPropertyAtSlot(
    JSContext* cx, JS::Handle<SharedShape*> shape, JS::HandleId id,
    uint8_t flags, uint32_t slot, SharedShape** result) {
  return SharedShape::getShapeWithPropertyAtSlot(
      cx, shape, id, PropertyFlags::fromRaw(flags), slot, result);
}

JS_PUBLIC_API bool js::ExternalAddPropertyWithShape(
    JSContext* cx, JS::Handle<NativeObject*> obj, SharedShape* newShape,
    uint32_t* slot) {
  return NativeObject::addPropertyWithShape(cx, obj, newShape, slot);
}
