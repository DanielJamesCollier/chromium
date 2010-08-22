// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WEBKIT_GLUE_PLUGINS_PEPPER_CHAR_SET_H_
#define WEBKIT_GLUE_PLUGINS_PEPPER_CHAR_SET_H_

typedef struct _ppb_CharSetDev PPB_CharSetDev;

namespace pepper {

class CharSet {
 public:
  // Returns a pointer to the interface implementing PPB_CharSet that is
  // exposed to the plugin.
  static const PPB_CharSetDev* GetInterface();
};

}  // namespace pepper

#endif  // WEBKIT_GLUE_PLUGINS_PEPPER_CHAR_SET_H_
