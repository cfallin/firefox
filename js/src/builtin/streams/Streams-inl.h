//
// Created by Till Schneidereit on 25.07.25.
//

#ifndef STREAMS_INL_H
#define STREAMS_INL_H

#include "vm/Compartment-inl.h"

namespace js {

extern JS_PUBLIC_API JSObject* UnwrapReadableStream(JSObject* obj);

/**
 * Read a private slot that is known to point to a particular type of object.
 *
 * Some internal slots specified in various standards effectively have static
 * types. For example, the [[ownerReadableStream]] slot of a stream reader is
 * guaranteed to be a ReadableStream. However, because of compartments, we
 * sometimes store a cross-compartment wrapper in that slot. And since wrappers
 * can be nuked, that wrapper may become a dead object proxy.
 *
 * UnwrapInternalSlot() copes with the cross-compartment and dead object cases,
 * but not plain bugs where the slot hasn't been initialized or doesn't contain
 * the expected type of object. Call this only if the slot is certain to
 * contain either an instance of T, a wrapper for a T, or a dead object.
 *
 * `cx` and `unwrappedObj` are not required to be same-compartment.
 *
 * DANGER: The result may not be same-compartment with either `cx` or `obj`.
 */
template <class T>
[[nodiscard]] inline T* UnwrapInternalSlot(JSContext* cx,
                                           Handle<NativeObject*> unwrappedObj,
                                           uint32_t slot) {
  static_assert(!std::is_convertible_v<T*, Wrapper*>,
                "T can't be a Wrapper type; this function discards wrappers");

  return UnwrapAndDowncastValue<T>(cx, unwrappedObj->getFixedSlot(slot));
}

/**
 * Read a function slot that is known to point to a particular type of object.
 *
 * This is like UnwrapInternalSlot, but for extended function slots. Call this
 * only if the specified slot is known to have been initialized with an object
 * of class T or a wrapper for such an object.
 *
 * DANGER: The result may not be same-compartment with `cx`.
 */
template <class T>
[[nodiscard]] T* UnwrapCalleeSlot(JSContext* cx, CallArgs& args,
                                  size_t extendedSlot) {
  JSFunction& func = args.callee().as<JSFunction>();
  return UnwrapAndDowncastValue<T>(cx, func.getExtendedSlot(extendedSlot));
}

}  // namespace js

#endif //STREAMS_INL_H
