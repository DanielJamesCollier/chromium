// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_SPEECH_RECOGNITION_EVENT_LISTENER_H_
#define CONTENT_PUBLIC_BROWSER_SPEECH_RECOGNITION_EVENT_LISTENER_H_

#include "base/basictypes.h"
#include "content/common/content_export.h"
#include "content/public/common/speech_recognition_result.h"

namespace content {

// The interface to be implemented by consumers interested in receiving
// speech recognition events.
class CONTENT_EXPORT SpeechRecognitionEventListener {
 public:
  // Invoked when the StartRequest is received and the recognition process is
  // started.
  virtual void OnRecognitionStart(int caller_id_id) = 0;

  // Invoked when the first audio capture is initiated.
  virtual void OnAudioStart(int caller_id) = 0;

  // At the start of recognition, a short amount of audio is recorded to
  // estimate the environment/background noise and this callback is issued
  // after that is complete. Typically the delegate brings up any speech
  // recognition UI once this callback is received.
  virtual void OnEnvironmentEstimationComplete(int caller_id) = 0;

  // Informs that the end pointer has started detecting sound (possibly speech).
  virtual void OnSoundStart(int caller_id) = 0;

  // Informs that the end pointer has stopped detecting sound (a long silence).
  virtual void OnSoundEnd(int caller_id) = 0;

  // Invoked when audio capture stops, either due to the end pointer detecting
  // silence, an internal error, or an explicit stop was issued.
  virtual void OnAudioEnd(int caller_id) = 0;

  // Invoked when a result is retrieved.
  virtual void OnRecognitionResult(int caller_id,
                                   const SpeechRecognitionResult& result) = 0;

  // Invoked if there was an error while capturing or recognizing audio.
  // The recognition has already been cancelled when this call is made and
  // no more events will be raised.
  virtual void OnRecognitionError(int caller_id,
                                  const SpeechRecognitionErrorCode& error) = 0;

  // Informs of a change in the captured audio level, useful if displaying
  // a microphone volume indicator while recording.
  // The value of |volume| and |noise_volume| is in the [0.0, 1.0] range.
  virtual void OnAudioLevelsChange(int caller_id, float volume,
                                   float noise_volume) = 0;

  // This is guaranteed to be the last event raised in the recognition
  // process and the |SpeechRecognizer| object can be freed if necessary.
  virtual void OnRecognitionEnd(int caller_id) = 0;

protected:
  virtual ~SpeechRecognitionEventListener() { }
};

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_SPEECH_RECOGNITION_EVENT_LISTENER_H_
