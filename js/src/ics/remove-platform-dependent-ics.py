#!/usr/bin/env python3

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import glob
import os

for file in glob.glob("IC-*"):
    with open(file, "rb") as f:
        content = f.read()
        if b'CallScriptedProxy' in content:
            print("Removing: %s" % file)
            os.unlink(file)
