// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_AUTOFILL_AUTOFILL_DIALOG_H_
#define CHROME_BROWSER_AUTOFILL_AUTOFILL_DIALOG_H_

#include <vector>

#include "chrome/browser/autofill/autofill_profile.h"
#include "chrome/browser/autofill/credit_card.h"
#include "gfx/native_widget_types.h"

class Profile;

// An interface the AutoFill dialog uses to notify its clients (observers) when
// the user has applied changes to the AutoFill profile data.
class AutoFillDialogObserver {
 public:
  // The user has confirmed changes by clicking "Apply" or "OK".  Any of the
  // parameters may be NULL.
  virtual void OnAutoFillDialogApply(
      std::vector<AutoFillProfile>* profiles,
      std::vector<CreditCard>* credit_cards) = 0;

 protected:
  virtual ~AutoFillDialogObserver() {}
};

// Shows the AutoFill dialog, which allows the user to edit profile information.
// |profile| is profile from which you can get vectors of of autofill profiles
// that contains the current profile information and credit cards.
// The dialog fills out the profile fields using this data. |observer| will be
// notified by OnAutoFillDialogAccept when the user has applied changes.
//
// The |parent| parameter (currently only used on Windows) specifies the parent
// view in the view hierarchy.  May be NULL on Mac and gtk.
//
// Optional parameters |imported_profile| and |imported_credit_card| may be
// supplied.  If they are supplied (non-NULL) they will be used instead of
// the profile and credit card data retrieved from the PersonalDataManager
// associated with the |profile|.
//
// The PersonalDataManager owns the contents of these vectors.  The lifetime of
// the contents is until the PersonalDataManager replaces them with new data
// whenever the web database is updated.
void ShowAutoFillDialog(gfx::NativeView parent,
                        AutoFillDialogObserver* observer,
                        Profile* profile,
                        AutoFillProfile* imported_profile,
                        CreditCard* imported_credit_card);

#endif  // CHROME_BROWSER_AUTOFILL_AUTOFILL_DIALOG_H_
