// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_BROWSING_DATA_DATABASE_HELPER_H_
#define CHROME_BROWSER_BROWSING_DATA_DATABASE_HELPER_H_

#include <string>
#include <vector>

#include "base/scoped_ptr.h"
#include "base/task.h"

class Profile;

// This class fetches database information in the FILE thread, and notifies the
// UI thread upon completion.
// A client of this class need to call StartFetching from the UI thread to
// initiate the flow, and it'll be notified by the callback in its UI
// thread at some later point.
// The client must call CancelNotification() if it's destroyed before the
// callback is notified.
class BrowsingDataDatabaseHelper
    : public base::RefCountedThreadSafe<BrowsingDataDatabaseHelper> {
 public:
  // Contains detailed information about a web database.
  struct DatabaseInfo {
    DatabaseInfo() {}
    DatabaseInfo(const std::string& host,
                 const std::string& database_name,
                 const std::string& origin_identifier,
                 const std::string& description,
                 int64 size,
                 base::Time last_modified)
        : host(host),
          database_name(database_name),
          origin_identifier(origin_identifier),
          description(description),
          size(size),
          last_modified(last_modified) {
    }

    std::string host;
    std::string database_name;
    std::string origin_identifier;
    std::string description;
    int64 size;
    base::Time last_modified;
  };

  explicit BrowsingDataDatabaseHelper(Profile* profile);

  // Starts the fetching process, which will notify its completion via
  // callback.
  // This must be called only in the UI thread.
  virtual void StartFetching(
      Callback1<const std::vector<DatabaseInfo>& >::Type* callback);

  // Cancels the notification callback (i.e., the window that created it no
  // longer exists).
  // This must be called only in the UI thread.
  virtual void CancelNotification();

  // Requests a single database to be deleted in the FILE thread. This must be
  // called in the UI thread.
  virtual void DeleteDatabase(const std::string& origin,
                              const std::string& name);

 private:
  friend class base::RefCountedThreadSafe<BrowsingDataDatabaseHelper>;
  friend class MockBrowsingDataDatabaseHelper;

  virtual ~BrowsingDataDatabaseHelper();

  // Enumerates all databases. This must be called in the FILE thread.
  void FetchDatabaseInfoInFileThread();

  // Notifies the completion callback. This must be called in the UI thread.
  void NotifyInUIThread();

  // Delete a single database file. This must be called in the FILE thread.
  void DeleteDatabaseInFileThread(const std::string& origin,
                                  const std::string& name);

  Profile* profile_;

  // This only mutates on the UI thread.
  scoped_ptr<Callback1<const std::vector<DatabaseInfo>& >::Type >
      completion_callback_;

  // Indicates whether or not we're currently fetching information:
  // it's true when StartFetching() is called in the UI thread, and it's reset
  // after we notify the callback in the UI thread.
  // This only mutates on the UI thread.
  bool is_fetching_;

  // This only mutates in the FILE thread.
  std::vector<DatabaseInfo> database_info_;

  DISALLOW_COPY_AND_ASSIGN(BrowsingDataDatabaseHelper);
};

#endif  // CHROME_BROWSER_BROWSING_DATA_DATABASE_HELPER_H_
