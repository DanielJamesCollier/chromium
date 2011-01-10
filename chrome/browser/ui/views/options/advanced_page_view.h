// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_OPTIONS_ADVANCED_PAGE_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_OPTIONS_ADVANCED_PAGE_VIEW_H_
#pragma once

#include "chrome/browser/ui/views/options/options_page_view.h"
#include "views/controls/button/button.h"

class AdvancedOptionsListModel;
class AdvancedScrollViewContainer;
class PrefService;
namespace views {
class NativeButton;
}

///////////////////////////////////////////////////////////////////////////////
// AdvancedPageView

class AdvancedPageView : public OptionsPageView,
                         public views::ButtonListener {
 public:
  explicit AdvancedPageView(Profile* profile);
  virtual ~AdvancedPageView();

  // Resets all prefs to their default values.
  void ResetToDefaults();

  // views::ButtonListener implementation:
  virtual void ButtonPressed(views::Button* sender, const views::Event& event);

 protected:
  // OptionsPageView implementation:
  virtual void InitControlLayout();

 private:
  // Controls for the Advanced page
  AdvancedScrollViewContainer* advanced_scroll_view_;
  views::NativeButton* reset_to_default_button_;

  DISALLOW_COPY_AND_ASSIGN(AdvancedPageView);
};

#endif  // CHROME_BROWSER_UI_VIEWS_OPTIONS_ADVANCED_PAGE_VIEW_H_
