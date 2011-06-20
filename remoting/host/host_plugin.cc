// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "base/at_exit.h"
#include "base/base_paths.h"
#include "base/basictypes.h"
#include "base/bind.h"
#include "base/file_path.h"
#include "base/logging.h"
#include "base/message_loop.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/stringize_macros.h"
#include "base/task.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/synchronization/cancellation_flag.h"
#include "remoting/base/auth_token_util.h"
#include "remoting/host/chromoting_host.h"
#include "remoting/host/chromoting_host_context.h"
#include "remoting/host/host_config.h"
#include "remoting/host/host_key_pair.h"
#include "remoting/host/host_status_observer.h"
#include "remoting/host/in_memory_host_config.h"
#include "remoting/host/register_support_host_request.h"
#include "remoting/host/support_access_verifier.h"
#include "third_party/npapi/bindings/npapi.h"
#include "third_party/npapi/bindings/npfunctions.h"
#include "third_party/npapi/bindings/npruntime.h"

// Symbol export is handled with a separate def file on Windows.
#if defined (__GNUC__) && __GNUC__ >= 4
#define EXPORT __attribute__((visibility("default")))
#else
#define EXPORT
#endif

#if defined(OS_WIN)
// TODO(wez): libvpx expects these 64-bit division functions to be provided
// by libgcc.a, which we aren't linked against.  These implementations can
// be removed once we have native MSVC libvpx builds for Windows.
extern "C" {

int64_t __cdecl __divdi3(int64_t a, int64_t b) {
  return a / b;
}
uint64_t __cdecl __udivdi3(uint64_t a, uint64_t b) {
  return a / b;
}

}
#endif

// Supported Javascript interface:
// readonly attribute string accessCode;
// readonly attribute int state;
//
// state: {
//     DISCONNECTED,
//     REQUESTED_ACCESS_CODE,
//     RECEIVED_ACCESS_CODE,
//     CONNECTED,
//     AFFIRMING_CONNECTION,
//     ERROR,
// }
//
// attribute Function void logDebugInfo(string);
// attribute Function void onStateChanged();
//
// // The |auth_service_with_token| parameter should be in the format
// // "auth_service:auth_token".  An example would be "oauth2:1/2a3912vd".
// void connect(string uid, string auth_service_with_token);
// void disconnect();


namespace {

const char* kAttrNameAccessCode = "accessCode";
const char* kAttrNameState = "state";
const char* kAttrNameLogDebugInfo = "logDebugInfo";
const char* kAttrNameOnStateChanged = "onStateChanged";
const char* kFuncNameConnect = "connect";
const char* kFuncNameDisconnect = "disconnect";

// States.
const char* kAttrNameDisconnected = "DISCONNECTED";
const char* kAttrNameRequestedAccessCode = "REQUESTED_ACCESS_CODE";
const char* kAttrNameReceivedAccessCode = "RECEIVED_ACCESS_CODE";
const char* kAttrNameConnected = "CONNECTED";
const char* kAttrNameAffirmingConnection = "AFFIRMING_CONNECTION";
const char* kAttrNameError = "ERROR";

const int kMaxLoginAttempts = 5;

// Global netscape functions initialized in NP_Initialize.
NPNetscapeFuncs* g_npnetscape_funcs = NULL;

// Global AtExitManager, created in NP_Initialize and destroyed in NP_Shutdown.
base::AtExitManager* g_at_exit_manager = NULL;

// The name and description are returned by GetValue, but are also
// combined with the MIME type to satisfy GetMIMEDescription, so we
// use macros here to allow that to happen at compile-time.
#define HOST_PLUGIN_NAME "Remoting Host Plugin"
#define HOST_PLUGIN_DESCRIPTION "Remoting Host Plugin"

// Convert an NPIdentifier into a std::string.
std::string StringFromNPIdentifier(NPIdentifier identifier) {
  if (!g_npnetscape_funcs->identifierisstring(identifier))
    return std::string();
  NPUTF8* np_string = g_npnetscape_funcs->utf8fromidentifier(identifier);
  std::string string(np_string);
  g_npnetscape_funcs->memfree(np_string);
  return string;
}

// Convert an NPVariant into a std::string.
std::string StringFromNPVariant(const NPVariant& variant) {
  if (!NPVARIANT_IS_STRING(variant))
    return std::string();
  const NPString& np_string = NPVARIANT_TO_STRING(variant);
  return std::string(np_string.UTF8Characters, np_string.UTF8Length);
}

// Convert a std::string into an NPVariant.
// Caller is responsible for making sure that NPN_ReleaseVariantValue is
// called on returned value.
NPVariant NPVariantFromString(const std::string& val) {
  size_t len = val.length();
  NPUTF8* chars =
      reinterpret_cast<NPUTF8*>(g_npnetscape_funcs->memalloc(len + 1));
  strcpy(chars, val.c_str());
  NPVariant variant;
  STRINGN_TO_NPVARIANT(chars, len, variant);
  return variant;
}

// Convert an NPVariant into an NSPObject.
NPObject* ObjectFromNPVariant(const NPVariant& variant) {
  if (!NPVARIANT_IS_OBJECT(variant))
    return NULL;
  return NPVARIANT_TO_OBJECT(variant);
}

// NPAPI plugin implementation for remoting host script object.
// HostNPScriptObject creates threads that are required to run
// ChromotingHost and starts/stops the host on those threads. When
// destroyed it sychronously shuts down the host and all threads.
class HostNPScriptObject : public remoting::HostStatusObserver {
 public:
  HostNPScriptObject(NPP plugin, NPObject* parent)
      : plugin_(plugin),
        parent_(parent),
        state_(kDisconnected),
        log_debug_info_func_(NULL),
        on_state_changed_func_(NULL),
        np_thread_id_(base::PlatformThread::CurrentId()),
        failed_login_attempts_(0),
        disconnected_event_(true, false) {
    VLOG(2) << "HostNPScriptObject";
    host_context_.SetUITaskPostFunction(base::Bind(
        &HostNPScriptObject::PostTaskToNPThread, base::Unretained(this)));
  }

  ~HostNPScriptObject() {
    CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);

    // Disconnect synchronously. We cannot disconnect asynchronously
    // here because |host_context_| needs to be stopped on the plugin
    // thread, but the plugin thread may not exist after the instance
    // is destroyed.
    destructing_.Set();
    disconnected_event_.Reset();
    DisconnectInternal();
    disconnected_event_.Wait();

    host_context_.Stop();
    if (log_debug_info_func_) {
      g_npnetscape_funcs->releaseobject(log_debug_info_func_);
    }
    if (on_state_changed_func_) {
      g_npnetscape_funcs->releaseobject(on_state_changed_func_);
    }
  }

  bool Init() {
    VLOG(2) << "Init";
    // TODO(wez): This starts a bunch of threads, which might fail.
    host_context_.Start();
    return true;
  }

  bool HasMethod(const std::string& method_name) {
    VLOG(2) << "HasMethod " << method_name;
    CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);
    return (method_name == kFuncNameConnect ||
            method_name == kFuncNameDisconnect);
  }

  bool InvokeDefault(const NPVariant* args,
                     uint32_t argCount,
                     NPVariant* result) {
    VLOG(2) << "InvokeDefault";
    CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);
    SetException("exception during default invocation");
    return false;
  }

  bool Invoke(const std::string& method_name,
              const NPVariant* args,
              uint32_t argCount,
              NPVariant* result) {
    VLOG(2) << "Invoke " << method_name;
    CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);
    if (method_name == kFuncNameConnect) {
      return Connect(args, argCount, result);
    } else if (method_name == kFuncNameDisconnect) {
      return Disconnect(args, argCount, result);
    } else {
      SetException("Invoke: unknown method " + method_name);
      return false;
    }
  }

  bool HasProperty(const std::string& property_name) {
    VLOG(2) << "HasProperty " << property_name;
    CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);
    return (property_name == kAttrNameAccessCode ||
            property_name == kAttrNameState ||
            property_name == kAttrNameLogDebugInfo ||
            property_name == kAttrNameOnStateChanged ||
            property_name == kAttrNameDisconnected ||
            property_name == kAttrNameRequestedAccessCode ||
            property_name == kAttrNameReceivedAccessCode ||
            property_name == kAttrNameConnected ||
            property_name == kAttrNameAffirmingConnection ||
            property_name == kAttrNameError);
  }

  bool GetProperty(const std::string& property_name, NPVariant* result) {
    VLOG(2) << "GetProperty " << property_name;
    CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);
    if (!result) {
      SetException("GetProperty: NULL result");
      return false;
    }

    if (property_name == kAttrNameOnStateChanged) {
      OBJECT_TO_NPVARIANT(on_state_changed_func_, *result);
      return true;
    } else if (property_name == kAttrNameLogDebugInfo) {
      OBJECT_TO_NPVARIANT(log_debug_info_func_, *result);
      return true;
    } else if (property_name == kAttrNameState) {
      INT32_TO_NPVARIANT(state_, *result);
      return true;
    } else if (property_name == kAttrNameAccessCode) {
      *result = NPVariantFromString(access_code_);
      return true;
    } else if (property_name == kAttrNameDisconnected) {
      INT32_TO_NPVARIANT(kDisconnected, *result);
      return true;
    } else if (property_name == kAttrNameRequestedAccessCode) {
      INT32_TO_NPVARIANT(kRequestedAccessCode, *result);
      return true;
    } else if (property_name == kAttrNameReceivedAccessCode) {
      INT32_TO_NPVARIANT(kReceivedAccessCode, *result);
      return true;
    } else if (property_name == kAttrNameConnected) {
      INT32_TO_NPVARIANT(kConnected, *result);
      return true;
    } else if (property_name == kAttrNameAffirmingConnection) {
      INT32_TO_NPVARIANT(kAffirmingConnection, *result);
      return true;
    } else if (property_name == kAttrNameError) {
      INT32_TO_NPVARIANT(kError, *result);
      return true;
    } else {
      SetException("GetProperty: unsupported property " + property_name);
      return false;
    }
  }

  bool SetProperty(const std::string& property_name, const NPVariant* value) {
    VLOG(2) << "SetProperty " << property_name;
    CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);

    if (property_name == kAttrNameOnStateChanged) {
      if (NPVARIANT_IS_OBJECT(*value)) {
        if (on_state_changed_func_) {
          g_npnetscape_funcs->releaseobject(on_state_changed_func_);
        }
        on_state_changed_func_ = NPVARIANT_TO_OBJECT(*value);
        if (on_state_changed_func_) {
          g_npnetscape_funcs->retainobject(on_state_changed_func_);
        }
        return true;
      } else {
        SetException("SetProperty: unexpected type for property " +
                     property_name);
      }
      return false;
    }

    if (property_name == kAttrNameLogDebugInfo) {
      if (NPVARIANT_IS_OBJECT(*value)) {
        if (log_debug_info_func_) {
          g_npnetscape_funcs->releaseobject(log_debug_info_func_);
        }
        log_debug_info_func_ = NPVARIANT_TO_OBJECT(*value);
        if (log_debug_info_func_) {
          g_npnetscape_funcs->retainobject(log_debug_info_func_);
        }
        return true;
      } else {
        SetException("SetProperty: unexpected type for property " +
                     property_name);
      }
      return false;
    }

    return false;
  }

  bool RemoveProperty(const std::string& property_name) {
    VLOG(2) << "RemoveProperty " << property_name;
    CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);
    return false;
  }

  bool Enumerate(std::vector<std::string>* values) {
    VLOG(2) << "Enumerate";
    CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);
    const char* entries[] = {
      kAttrNameAccessCode,
      kAttrNameState,
      kAttrNameLogDebugInfo,
      kAttrNameOnStateChanged,
      kFuncNameConnect,
      kFuncNameDisconnect,
      kAttrNameDisconnected,
      kAttrNameRequestedAccessCode,
      kAttrNameReceivedAccessCode,
      kAttrNameConnected,
      kAttrNameAffirmingConnection,
      kAttrNameError
    };
    for (size_t i = 0; i < arraysize(entries); ++i) {
      values->push_back(entries[i]);
    }
    return true;
  }

  // remoting::HostStatusObserver implementation.
  virtual void OnSignallingConnected(remoting::SignalStrategy* signal_strategy,
                                     const std::string& full_jid) OVERRIDE {
    OnStateChanged(kConnected);
  }

  virtual void OnSignallingDisconnected() OVERRIDE {
  }

  virtual void OnAccessDenied() OVERRIDE {
    DCHECK_EQ(MessageLoop::current(), host_context_.network_message_loop());

    ++failed_login_attempts_;
    if (failed_login_attempts_ == kMaxLoginAttempts)
      DisconnectInternal();
  }

  virtual void OnShutdown() OVERRIDE {
    DCHECK_EQ(MessageLoop::current(), host_context_.main_message_loop());

    OnStateChanged(kDisconnected);
  }

 private:
  enum State {
    kDisconnected,
    kRequestedAccessCode,
    kReceivedAccessCode,
    kConnected,
    kAffirmingConnection,
    kError
  };

  // Start connection. args are:
  //   string uid, string auth_token
  // No result.
  bool Connect(const NPVariant* args, uint32_t argCount, NPVariant* result);

  // Disconnect. No arguments or result.
  bool Disconnect(const NPVariant* args, uint32_t argCount, NPVariant* result);

  // Call LogDebugInfo handler if there is one.
  void LogDebugInfo(const std::string& message);

  // Call OnStateChanged handler if there is one.
  void OnStateChanged(State state);

  // Callbacks invoked during session setup.
  void OnReceivedSupportID(remoting::SupportAccessVerifier* access_verifier,
                           bool success,
                           const std::string& support_id);

  // Helper functions that run on main thread. Can be called on any
  // other thread.
  void ConnectInternal(const std::string& uid,
                       const std::string& auth_token,
                       const std::string& auth_service);
  void DisconnectInternal();

  // Callback for ChromotingHost::Shutdown().
  void OnShutdownFinished();

  // Call a JavaScript function wrapped as an NPObject.
  // If result is non-null, the result of the call will be stored in it.
  // Caller is responsible for releasing result if they ask for it.
  static bool CallJSFunction(NPObject* func,
                             const NPVariant* args,
                             uint32_t argCount,
                             NPVariant* result);

  // Posts a task on the main NP thread.
  void PostTaskToNPThread(const tracked_objects::Location& from_here,
                          Task* task);

  // Utility function for PostTaskToNPThread.
  static void NPTaskSpringboard(void* task);

  // Set an exception for the current call.
  void SetException(const std::string& exception_string);

  NPP plugin_;
  NPObject* parent_;
  int state_;
  std::string access_code_;
  NPObject* log_debug_info_func_;
  NPObject* on_state_changed_func_;
  base::PlatformThreadId np_thread_id_;

  scoped_ptr<remoting::RegisterSupportHostRequest> register_request_;
  scoped_refptr<remoting::ChromotingHost> host_;
  scoped_refptr<remoting::MutableHostConfig> host_config_;
  remoting::ChromotingHostContext host_context_;
  int failed_login_attempts_;

  base::WaitableEvent disconnected_event_;
  base::CancellationFlag destructing_;
};

// string uid, string auth_token
bool HostNPScriptObject::Connect(const NPVariant* args,
                                 uint32_t arg_count,
                                 NPVariant* result) {
  LogDebugInfo("Connecting...");

  CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);
  if (arg_count != 2) {
    SetException("connect: bad number of arguments");
    return false;
  }

  std::string uid = StringFromNPVariant(args[0]);
  if (uid.empty()) {
    SetException("connect: bad uid argument");
    return false;
  }

  std::string auth_service_with_token = StringFromNPVariant(args[1]);
  std::string auth_token;
  std::string auth_service;
  remoting::ParseAuthTokenWithService(auth_service_with_token, &auth_token,
                                      &auth_service);
  if (auth_token.empty()) {
    SetException("connect: auth_service_with_token argument has empty token");
    return false;
  }

  ConnectInternal(uid, auth_token, auth_service);

  return true;
}

void HostNPScriptObject::ConnectInternal(
    const std::string& uid,
    const std::string& auth_token,
    const std::string& auth_service) {
  if (MessageLoop::current() != host_context_.main_message_loop()) {
    host_context_.main_message_loop()->PostTask(
        FROM_HERE,
        NewRunnableMethod(this, &HostNPScriptObject::ConnectInternal,
                          uid, auth_token, auth_service));
    return;
  }
  // Store the supplied user ID and token to the Host configuration.
  scoped_refptr<remoting::MutableHostConfig> host_config =
      new remoting::InMemoryHostConfig;
  host_config->SetString(remoting::kXmppLoginConfigPath, uid);
  host_config->SetString(remoting::kXmppAuthTokenConfigPath, auth_token);
  host_config->SetString(remoting::kXmppAuthServiceConfigPath, auth_service);

  // Create an access verifier and fetch the host secret.
  scoped_ptr<remoting::SupportAccessVerifier> access_verifier;
  access_verifier.reset(new remoting::SupportAccessVerifier);

  // Generate a key pair for the Host to use.
  // TODO(wez): Move this to the worker thread.
  remoting::HostKeyPair host_key_pair;
  host_key_pair.Generate();
  host_key_pair.Save(host_config);

  // Request registration of the host for support.
  scoped_ptr<remoting::RegisterSupportHostRequest> register_request(
      new remoting::RegisterSupportHostRequest());
  if (!register_request->Init(
          host_config.get(),
          base::Bind(&HostNPScriptObject::OnReceivedSupportID,
                     base::Unretained(this),
                     access_verifier.get()))) {
    OnStateChanged(kDisconnected);
    return;
  }

  // Create the Host.
  scoped_refptr<remoting::ChromotingHost> host =
      remoting::ChromotingHost::Create(&host_context_, host_config,
                                       access_verifier.release());
  host->AddStatusObserver(this);
  host->AddStatusObserver(register_request.get());
  host->set_it2me(true);

  // Nothing went wrong, so lets save the host, config and request.
  host_ = host;
  host_config_ = host_config;
  register_request_.reset(register_request.release());

  // Start the Host.
  host_->Start();

  OnStateChanged(kRequestedAccessCode);
  return;
}

bool HostNPScriptObject::Disconnect(const NPVariant* args,
                                    uint32_t arg_count,
                                    NPVariant* result) {
  CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);
  if (arg_count != 0) {
    SetException("disconnect: bad number of arguments");
    return false;
  }

  DisconnectInternal();

  return true;
}

void HostNPScriptObject::DisconnectInternal() {
  if (MessageLoop::current() != host_context_.main_message_loop()) {
    host_context_.main_message_loop()->PostTask(
        FROM_HERE,
        NewRunnableMethod(this, &HostNPScriptObject::DisconnectInternal));
    return;
  }

  if (!host_) {
    disconnected_event_.Signal();
    return;
  }

  host_->Shutdown(
      NewRunnableMethod(this, &HostNPScriptObject::OnShutdownFinished));
}

void HostNPScriptObject::OnShutdownFinished() {
  DCHECK_EQ(MessageLoop::current(), host_context_.main_message_loop());

  host_ = NULL;
  register_request_.reset();
  host_config_ = NULL;
  disconnected_event_.Signal();
}

void HostNPScriptObject::OnReceivedSupportID(
    remoting::SupportAccessVerifier* access_verifier,
    bool success,
    const std::string& support_id) {
  CHECK_NE(base::PlatformThread::CurrentId(), np_thread_id_);

  if (!success) {
    // TODO(wez): Replace the success/fail flag with full error reporting.
    DisconnectInternal();
    return;
  }

  // Inform the AccessVerifier of our Support-Id, for authentication.
  access_verifier->OnIT2MeHostRegistered(success, support_id);

  // Combine the Support Id with the Host Id to make the Access Code.
  // TODO(wez): Locking, anyone?
  access_code_ = support_id + "-" + access_verifier->host_secret();

  // Let the caller know that life is good.
  OnStateChanged(kReceivedAccessCode);
}

void HostNPScriptObject::OnStateChanged(State state) {
  if (destructing_.IsSet()) {
    return;
  }

  if (!host_context_.IsUIThread()) {
    host_context_.PostToUIThread(
        FROM_HERE,
        NewRunnableMethod(this, &HostNPScriptObject::OnStateChanged, state));
    return;
  }
  state_ = state;
  if (on_state_changed_func_) {
    VLOG(2) << "Calling state changed " << state;
    bool is_good = CallJSFunction(on_state_changed_func_, NULL, 0, NULL);
    LOG_IF(ERROR, !is_good) << "OnStateChangedNP failed";
  }
}

void HostNPScriptObject::LogDebugInfo(const std::string& message) {
  if (!host_context_.IsUIThread()) {
    host_context_.PostToUIThread(
        FROM_HERE,
        NewRunnableMethod(this, &HostNPScriptObject::LogDebugInfo, message));
    return;
  }
  if (log_debug_info_func_) {
    NPVariant* arg = new NPVariant();
    LOG(INFO) << "Logging: " << message;
    STRINGZ_TO_NPVARIANT(message.c_str(), *arg);
    bool is_good = CallJSFunction(log_debug_info_func_, arg, 1, NULL);
    LOG_IF(ERROR, !is_good) << "LogDebugInfo failed";
  }
}

void HostNPScriptObject::SetException(const std::string& exception_string) {
  CHECK_EQ(base::PlatformThread::CurrentId(), np_thread_id_);
  g_npnetscape_funcs->setexception(parent_, exception_string.c_str());
  LogDebugInfo(exception_string);
}

bool HostNPScriptObject::CallJSFunction(NPObject* func,
                                        const NPVariant* args,
                                        uint32_t argCount,
                                        NPVariant* result) {
  NPVariant np_result;
  bool is_good = func->_class->invokeDefault(func, args, argCount, &np_result);
  if (is_good) {
    if (result) {
      *result = np_result;
    } else {
      g_npnetscape_funcs->releasevariantvalue(&np_result);
    }
  }
  return is_good;
}

void HostNPScriptObject::PostTaskToNPThread(
    const tracked_objects::Location& from_here, Task* task) {
  // The NPAPI functions cannot make use of |from_here|, but this method is
  // passed as a callback to ChromotingHostContext, so it needs to have the
  // appropriate signature.

  // Can be called from any thread.
  g_npnetscape_funcs->pluginthreadasynccall(plugin_,
                                            &NPTaskSpringboard,
                                            task);
}

void HostNPScriptObject::NPTaskSpringboard(void* task) {
  Task* real_task = reinterpret_cast<Task*>(task);
  real_task->Run();
  delete real_task;
}

// NPAPI plugin implementation for remoting host.
// Documentation for most of the calls in this class can be found here:
// https://developer.mozilla.org/en/Gecko_Plugin_API_Reference/Scripting_plugins
class HostNPPlugin {
 public:
  // |mode| is the display mode of plug-in. Values:
  // NP_EMBED: (1) Instance was created by an EMBED tag and shares the browser
  //               window with other content.
  // NP_FULL: (2) Instance was created by a separate file and is the primary
  //              content in the window.
  HostNPPlugin(NPP instance, uint16 mode)
    : instance_(instance), scriptable_object_(NULL) {}

  ~HostNPPlugin() {
    if (scriptable_object_) {
      g_npnetscape_funcs->releaseobject(scriptable_object_);
      scriptable_object_ = NULL;
    }
  }

  bool Init(int16 argc, char** argn, char** argv, NPSavedData* saved) {
#if defined(OS_MACOSX)
    // Use the modern CoreGraphics and Cocoa models when available, since
    // QuickDraw and Carbon are deprecated.
    // The drawing and event models don't change anything for this plugin, since
    // none of the functions affected by the models actually do anything.
    // This does however keep the plugin from breaking when Chromium eventually
    // drops support for QuickDraw and Carbon, and it also keeps the browser
    // from sending Null Events once a second to support old Carbon based
    // timers.
    // Chromium should always be supporting the newer models.

    // Sanity check to see if Chromium supports the CoreGraphics drawing model.
    NPBool supports_core_graphics = false;
    NPError err = g_npnetscape_funcs->getvalue(instance_,
                                               NPNVsupportsCoreGraphicsBool,
                                               &supports_core_graphics);
    if (err == NPERR_NO_ERROR && supports_core_graphics) {
      // Switch to CoreGraphics drawing model.
      g_npnetscape_funcs->setvalue(instance_, NPPVpluginDrawingModel,
          reinterpret_cast<void*>(NPDrawingModelCoreGraphics));
    } else {
      LOG(ERROR) << "No Core Graphics support";
      return false;
    }

    // Sanity check to see if Chromium supports the Cocoa event model.
    NPBool supports_cocoa = false;
    err = g_npnetscape_funcs->getvalue(instance_, NPNVsupportsCocoaBool,
                                       &supports_cocoa);
    if (err == NPERR_NO_ERROR && supports_cocoa) {
      // Switch to Cocoa event model.
      g_npnetscape_funcs->setvalue(instance_, NPPVpluginEventModel,
          reinterpret_cast<void*>(NPEventModelCocoa));
    } else {
      LOG(ERROR) << "No Cocoa Event Model support";
      return false;
    }
#endif  // OS_MACOSX
    return true;
  }

  bool Save(NPSavedData** saved) {
    return true;
  }

  NPObject* GetScriptableObject() {
    if (!scriptable_object_) {
      // Must be static. If it is a temporary, objects created by this
      // method will fail in weird and wonderful ways later.
      static NPClass npc_ref_object = {
        NP_CLASS_STRUCT_VERSION,
        &Allocate,
        &Deallocate,
        &Invalidate,
        &HasMethod,
        &Invoke,
        &InvokeDefault,
        &HasProperty,
        &GetProperty,
        &SetProperty,
        &RemoveProperty,
        &Enumerate,
        NULL
      };
      scriptable_object_ = g_npnetscape_funcs->createobject(instance_,
                                                            &npc_ref_object);
    }
    return scriptable_object_;
  }

 private:
  struct ScriptableNPObject : public NPObject {
    HostNPScriptObject* scriptable_object;
  };

  static HostNPScriptObject* ScriptableFromObject(NPObject* obj) {
    return reinterpret_cast<ScriptableNPObject*>(obj)->scriptable_object;
  }

  static NPObject* Allocate(NPP npp, NPClass* aClass) {
    VLOG(2) << "static Allocate";
    ScriptableNPObject* object =
        reinterpret_cast<ScriptableNPObject*>(
            g_npnetscape_funcs->memalloc(sizeof(ScriptableNPObject)));

    object->_class = aClass;
    object->referenceCount = 1;
    object->scriptable_object = new HostNPScriptObject(npp, object);
    if (!object->scriptable_object->Init()) {
      Deallocate(object);
      object = NULL;
    }
    return object;
  }

  static void Deallocate(NPObject* npobj) {
    VLOG(2) << "static Deallocate";
    if (npobj) {
      Invalidate(npobj);
      g_npnetscape_funcs->memfree(npobj);
    }
  }

  static void Invalidate(NPObject* npobj) {
    if (npobj) {
      ScriptableNPObject* object = reinterpret_cast<ScriptableNPObject*>(npobj);
      if (object->scriptable_object) {
        delete object->scriptable_object;
        object->scriptable_object = NULL;
      }
    }
  }

  static bool HasMethod(NPObject* obj, NPIdentifier method_name) {
    VLOG(2) << "static HasMethod";
    HostNPScriptObject* scriptable = ScriptableFromObject(obj);
    if (!scriptable) return false;
    std::string method_name_string = StringFromNPIdentifier(method_name);
    if (method_name_string.empty())
      return false;
    return scriptable->HasMethod(method_name_string);
  }

  static bool InvokeDefault(NPObject* obj,
                            const NPVariant* args,
                            uint32_t argCount,
                            NPVariant* result) {
    VLOG(2) << "static InvokeDefault";
    HostNPScriptObject* scriptable = ScriptableFromObject(obj);
    if (!scriptable) return false;
    return scriptable->InvokeDefault(args, argCount, result);
  }

  static bool Invoke(NPObject* obj,
                     NPIdentifier method_name,
                     const NPVariant* args,
                     uint32_t argCount,
                     NPVariant* result) {
    VLOG(2) << "static Invoke";
    HostNPScriptObject* scriptable = ScriptableFromObject(obj);
    if (!scriptable)
      return false;
    std::string method_name_string = StringFromNPIdentifier(method_name);
    if (method_name_string.empty())
      return false;
    return scriptable->Invoke(method_name_string, args, argCount, result);
  }

  static bool HasProperty(NPObject* obj, NPIdentifier property_name) {
    VLOG(2) << "static HasProperty";
    HostNPScriptObject* scriptable = ScriptableFromObject(obj);
    if (!scriptable) return false;
    std::string property_name_string = StringFromNPIdentifier(property_name);
    if (property_name_string.empty())
      return false;
    return scriptable->HasProperty(property_name_string);
  }

  static bool GetProperty(NPObject* obj,
                          NPIdentifier property_name,
                          NPVariant* result) {
    VLOG(2) << "static GetProperty";
    HostNPScriptObject* scriptable = ScriptableFromObject(obj);
    if (!scriptable) return false;
    std::string property_name_string = StringFromNPIdentifier(property_name);
    if (property_name_string.empty())
      return false;
    return scriptable->GetProperty(property_name_string, result);
  }

  static bool SetProperty(NPObject* obj,
                          NPIdentifier property_name,
                          const NPVariant* value) {
    VLOG(2) << "static SetProperty";
    HostNPScriptObject* scriptable = ScriptableFromObject(obj);
    if (!scriptable) return false;
    std::string property_name_string = StringFromNPIdentifier(property_name);
    if (property_name_string.empty())
      return false;
    return scriptable->SetProperty(property_name_string, value);
  }

  static bool RemoveProperty(NPObject* obj, NPIdentifier property_name) {
    VLOG(2) << "static RemoveProperty";
    HostNPScriptObject* scriptable = ScriptableFromObject(obj);
    if (!scriptable) return false;
    std::string property_name_string = StringFromNPIdentifier(property_name);
    if (property_name_string.empty())
      return false;
    return scriptable->RemoveProperty(property_name_string);
  }

  static bool Enumerate(NPObject* obj,
                        NPIdentifier** value,
                        uint32_t* count) {
    VLOG(2) << "static Enumerate";
    HostNPScriptObject* scriptable = ScriptableFromObject(obj);
    if (!scriptable) return false;
    std::vector<std::string> values;
    bool is_good = scriptable->Enumerate(&values);
    if (is_good) {
      *count = values.size();
      *value = reinterpret_cast<NPIdentifier*>(
          g_npnetscape_funcs->memalloc(sizeof(NPIdentifier) * (*count)));
      for (uint32_t i = 0; i < *count; ++i) {
        (*value)[i] =
            g_npnetscape_funcs->getstringidentifier(values[i].c_str());
      }
    }
    return is_good;
  }

  NPP instance_;
  NPObject* scriptable_object_;
};

// Utility functions to map NPAPI Entry Points to C++ Objects.
HostNPPlugin* PluginFromInstance(NPP instance) {
  return reinterpret_cast<HostNPPlugin*>(instance->pdata);
}

NPError CreatePlugin(NPMIMEType pluginType,
                     NPP instance,
                     uint16 mode,
                     int16 argc,
                     char** argn,
                     char** argv,
                     NPSavedData* saved) {
  VLOG(2) << "CreatePlugin";
  HostNPPlugin* plugin = new HostNPPlugin(instance, mode);
  instance->pdata = plugin;
  if (!plugin->Init(argc, argn, argv, saved)) {
    delete plugin;
    instance->pdata = NULL;
    return NPERR_INVALID_PLUGIN_ERROR;
  } else {
    return NPERR_NO_ERROR;
  }
}

NPError DestroyPlugin(NPP instance,
                      NPSavedData** save) {
  VLOG(2) << "DestroyPlugin";
  HostNPPlugin* plugin = PluginFromInstance(instance);
  if (plugin) {
    plugin->Save(save);
    delete plugin;
    instance->pdata = NULL;
    return NPERR_NO_ERROR;
  } else {
    return NPERR_INVALID_PLUGIN_ERROR;
  }
}

NPError GetValue(NPP instance, NPPVariable variable, void* value) {
  switch(variable) {
  default:
    VLOG(2) << "GetValue - default " << variable;
    return NPERR_GENERIC_ERROR;
  case NPPVpluginNameString:
    VLOG(2) << "GetValue - name string";
    *reinterpret_cast<const char**>(value) = HOST_PLUGIN_NAME;
    break;
  case NPPVpluginDescriptionString:
    VLOG(2) << "GetValue - description string";
    *reinterpret_cast<const char**>(value) = HOST_PLUGIN_DESCRIPTION;
    break;
  case NPPVpluginNeedsXEmbed:
    VLOG(2) << "GetValue - NeedsXEmbed";
    *(static_cast<NPBool*>(value)) = true;
    break;
  case NPPVpluginScriptableNPObject:
    VLOG(2) << "GetValue - scriptable object";
    HostNPPlugin* plugin = PluginFromInstance(instance);
    if (!plugin)
      return NPERR_INVALID_PLUGIN_ERROR;
    NPObject* scriptable_object = plugin->GetScriptableObject();
    g_npnetscape_funcs->retainobject(scriptable_object);
    *reinterpret_cast<NPObject**>(value) = scriptable_object;
    break;
  }
  return NPERR_NO_ERROR;
}

NPError HandleEvent(NPP instance, void* ev) {
  VLOG(2) << "HandleEvent";
  return NPERR_NO_ERROR;
}

NPError SetWindow(NPP instance, NPWindow* pNPWindow) {
  VLOG(2) << "SetWindow";
  return NPERR_NO_ERROR;
}

}  // namespace

DISABLE_RUNNABLE_METHOD_REFCOUNT(HostNPScriptObject);

#if defined(OS_WIN)
HMODULE g_hModule = NULL;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
  switch (dwReason) {
    case DLL_PROCESS_ATTACH:
      g_hModule = hModule;
      DisableThreadLibraryCalls(hModule);
      break;
    case DLL_PROCESS_DETACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
      break;
  }
  return TRUE;
}
#endif

// The actual required NPAPI Entry points

extern "C" {

EXPORT NPError API_CALL NP_GetEntryPoints(NPPluginFuncs* nppfuncs) {
  VLOG(2) << "NP_GetEntryPoints";
  nppfuncs->version = (NP_VERSION_MAJOR << 8) | NP_VERSION_MINOR;
  nppfuncs->newp = &CreatePlugin;
  nppfuncs->destroy = &DestroyPlugin;
  nppfuncs->getvalue = &GetValue;
  nppfuncs->event = &HandleEvent;
  nppfuncs->setwindow = &SetWindow;

  return NPERR_NO_ERROR;
}

EXPORT NPError API_CALL NP_Initialize(NPNetscapeFuncs* npnetscape_funcs
#if defined(OS_POSIX) && !defined(OS_MACOSX)
                            , NPPluginFuncs* nppfuncs
#endif
                            ) {
  VLOG(2) << "NP_Initialize";
  if (g_at_exit_manager)
    return NPERR_MODULE_LOAD_FAILED_ERROR;

  if(npnetscape_funcs == NULL)
    return NPERR_INVALID_FUNCTABLE_ERROR;

  if(((npnetscape_funcs->version & 0xff00) >> 8) > NP_VERSION_MAJOR)
    return NPERR_INCOMPATIBLE_VERSION_ERROR;

  g_at_exit_manager = new base::AtExitManager;
  g_npnetscape_funcs = npnetscape_funcs;
#if defined(OS_POSIX) && !defined(OS_MACOSX)
  NP_GetEntryPoints(nppfuncs);
#endif
  return NPERR_NO_ERROR;
}

EXPORT NPError API_CALL NP_Shutdown() {
  VLOG(2) << "NP_Shutdown";
  delete g_at_exit_manager;
  g_at_exit_manager = NULL;
  return NPERR_NO_ERROR;
}

#if defined(OS_POSIX) && !defined(OS_MACOSX)
EXPORT const char* API_CALL NP_GetMIMEDescription(void) {
  VLOG(2) << "NP_GetMIMEDescription";
  return STRINGIZE(HOST_PLUGIN_MIME_TYPE) ":"
      HOST_PLUGIN_NAME ":"
      HOST_PLUGIN_DESCRIPTION;
}

EXPORT NPError API_CALL NP_GetValue(void* npp,
                                    NPPVariable variable,
                                    void* value) {
  return GetValue((NPP)npp, variable, value);
}
#endif

}  // extern "C"
