/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_NightMonkey_h
#define js_NightMonkey_h

#include "jstypes.h"

#include "js/RootingAPI.h"

struct JS_PUBLIC_API JSContext;

namespace JS {

extern JS_PUBLIC_API bool NightRegisterRoot(JSContext* cx,
                                            Handle<JSScript*> script,
                                            bool executedAtInit);

extern JS_PUBLIC_API bool NightCaptureSnapshotExtras(JSContext* cx,
                                                     Handle<JSScript*> root);

extern JS_PUBLIC_API bool NightCaptureSnapshotHeap(JSContext* cx);

extern JS_PUBLIC_API bool NightActivate(JSContext* cx);

}  // namespace JS

#endif  // js_NightMonkey_h
