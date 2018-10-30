/* Copyright 2018 Streampunk Media Ltd.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/* -LICENSE-START-
 ** Copyright (c) 2010 Blackmagic Design
 **
 ** Permission is hereby granted, free of charge, to any person or organization
 ** obtaining a copy of the software and accompanying documentation covered by
 ** this license (the "Software") to use, reproduce, display, distribute,
 ** execute, and transmit the Software, and to prepare derivative works of the
 ** Software, and to permit third-parties to whom the Software is furnished to
 ** do so, all subject to the following:
 **
 ** The copyright notices in the Software and this entire statement, including
 ** the above license grant, this restriction and the following disclaimer,
 ** must be included in all copies of the Software, in whole or in part, and
 ** all derivative works of the Software, unless such copies or derivative
 ** works are solely in the form of machine-executable object code generated by
 ** a source language processor.
 **
 ** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 ** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 ** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 ** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 ** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 ** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 ** DEALINGS IN THE SOFTWARE.
 ** -LICENSE-END-
 */

/* API sketch

let p = await mac.playback({

});

// Scheduled playback

p.scheduleVideo(data, time[o+0]);
p.scheduleAudio(data, time[o+0]);
p.scheduleVideo(data, time[0+1]);
p.scheduleAudio(data, time[o+1]);
p.scheduleVideo(data, time[o+2]);
p.scheduleAudio(data, time[o+2]);

lwt c = 0;
while (more) {
  let status = await p.played(time[c]);
  p.scheduleVideo(data, time[3+c]);
  p.scheduleAudio(data, time[3+c++]);
}

// Sync playback API

let f = await p.play(data);

*/

#include "playback_promise.h"

HRESULT playbackThreadsafe::ScheduledFrameCompleted(
  IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result) {

  return S_OK;
}

HRESULT playbackThreadsafe::ScheduledPlaybackHasStopped() {

  return S_OK;
}

void finalizePlaybackCarrier(napi_env env, void* finalize_data, void* finalize_hint) {
  printf("Finalizing playback threadsafe.\n");
  playbackThreadsafe* c = (playbackThreadsafe*) finalize_data;
  delete c;
}

void playbackExecute(napi_env env, void* data) {
  playbackCarrier* c = (playbackCarrier*) data;

  IDeckLinkIterator* deckLinkIterator;
  IDeckLink* deckLink;
  IDeckLinkOutput* deckLinkOutput;
  HRESULT hresult;

  #ifdef WIN32
  hresult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  CoCreateInstance(CLSID_CDeckLinkIterator, NULL, CLSCTX_ALL, IID_IDeckLinkIterator, (void**)&deckLinkIterator);
  #else
  deckLinkIterator = CreateDeckLinkIteratorInstance();
  #endif

  for ( uint32_t x = 0 ; x <= c->deviceIndex ; x++ ) {
    if (deckLinkIterator->Next(&deckLink) != S_OK) {
      printf("Falling out of device index iterator.\n");
      deckLinkIterator->Release();
      c->status = MACADAM_OUT_OF_BOUNDS;
      c->errorMsg = "Device index exceeds the number of installed devices.";
      return;
    }
  }

  deckLinkIterator->Release();

  if (deckLink->QueryInterface(IID_IDeckLinkOutput, (void **)&deckLinkOutput) != S_OK) {
    deckLink->Release();
    c->status = MACADAM_NO_OUTPUT;
    c->errorMsg = "Could not obtain the DeckLink Output interface. Does the device have an output?";
    return;
  }

  deckLink->Release();
  c->deckLinkOutput = deckLinkOutput;

  BMDDisplayModeSupport supported;

  hresult = deckLinkOutput->DoesSupportVideoMode(c->requestedDisplayMode,
    c->requestedPixelFormat, bmdVideoOutputFlagDefault,
    &supported, &c->selectedDisplayMode);
  if (hresult != S_OK) {
    c->status = MACADAM_CALL_FAILURE;
    c->errorMsg = "Unable to determine if video mode is supported by output device.";
    return;
  }
  switch (supported) {
    case bmdDisplayModeSupported:
      break;
    case bmdDisplayModeSupportedWithConversion:
      c->status = MACADAM_NO_CONVERESION; // TODO consider adding conversion support
      c->errorMsg = "Display mode is supported via conversion and not by macadam.";
      return;
    default:
      c->status = MACADAM_MODE_NOT_SUPPORTED;
      c->errorMsg = "Requested display mode is not supported.";
      return;
  }

  hresult = deckLinkOutput->EnableVideoOutput(
    c->requestedDisplayMode, bmdVideoOutputFlagDefault);
  switch (hresult) {
    case E_INVALIDARG: // Should have been picked up by DoesSupportVideoMode
      c->status = MACADAM_INVALID_ARGS;
      c->errorMsg = "Invalid arguments used to enable video output.";
      return;
    case E_ACCESSDENIED:
      c->status = MACADAM_ACCESS_DENIED;
      c->errorMsg = "Unable to access the hardware or input stream is currently active.";
      return;
    case E_OUTOFMEMORY:
      c->status = MACADAM_OUT_OF_MEMORY;
      c->errorMsg = "Unable to create an output video frame - out of memory.";
      return;
    case E_FAIL:
      c->status = MACADAM_CALL_FAILURE;
      c->errorMsg = "Failed to enable video input.";
      return;
    case S_OK:
      break;
  }

  if (c->channels > 0) {
    hresult = deckLinkOutput->EnableAudioOutput(c->requestedSampleRate,
      c->requestedSampleType, c->channels, bmdAudioOutputStreamTimestamped);
    switch (hresult)  {
      case E_INVALIDARG:
        c->status = MACADAM_INVALID_ARGS;
        c->errorMsg = "Invalid arguments used to enable audio output. BMD supports 48kHz, 16- or 32-bit integer only.";
        return;
      case E_FAIL:
        c->status = MACADAM_CALL_FAILURE;
        c->errorMsg = "Failed to enable audio input.";
        return;
      case E_ACCESSDENIED:
        c->status = MACADAM_ACCESS_DENIED;
        c->errorMsg = "Unable to access the hardware or audio output is not enabled.";
        return;
      case E_OUTOFMEMORY:
        c->status = MACADAM_OUT_OF_MEMORY;
        c->errorMsg = "Unable to create a new internal audio frame - out of memory.";
        return;
      case S_OK:
        break;
    }
  }
}

void playbackComplete(napi_env env, napi_status asyncStatus, void* data) {
  napi_value param, paramPart, result, asyncName;
  BMDTimeValue frameRateDuration;
  BMDTimeScale frameRateScale;
  HRESULT hresult;

  playbackCarrier* c = (playbackCarrier*) data;

  if (asyncStatus != napi_ok) {
    c->status = asyncStatus;
    c->errorMsg = "Async capture creator failed to complete.";
  }
  REJECT_STATUS;

  c->status = napi_create_object(env, &result);
  REJECT_STATUS;
  c->status = napi_create_string_utf8(env, "playback", NAPI_AUTO_LENGTH, &param);
  REJECT_STATUS;
  c->status = napi_set_named_property(env, result, "type", param);
  REJECT_STATUS;


  #ifdef WIN32
  BSTR displayModeBSTR = NULL;
  hresult = c->selectedDisplayMode->GetName(&displayModeBSTR);
  if (hresult == S_OK) {
    _bstr_t deviceName(displayModeBSTR, false);
    c->status = napi_create_string_utf8(env, (char*) deviceName, NAPI_AUTO_LENGTH, &param);
    REJECT_STATUS;
  }
  #elif __APPLE__
  CFStringRef displayModeCFString = NULL;
  hresult = c->selectedDisplayMode->GetName(&displayModeCFString);
  if (hresult == S_OK) {
    char displayModeName[64];
    CFStringGetCString(displayModeCFString, displayModeName, sizeof(displayModeName), kCFStringEncodingMacRoman);
    CFRelease(displayModeCFString);
    c->status = napi_create_string_utf8(env, displayModeName, NAPI_AUTO_LENGTH, &param);
    REJECT_STATUS;
  }
  #else
  char* displayModeName;
  hresult = c->selectedDisplayMode->GetName((const char **) &displayModeName);
  if (hresult == S_OK) {
    c->status = napi_create_string_utf8(env, displayModeName, NAPI_AUTO_LENGTH, &param);
    free(displayModeName);
    REJECT_STATUS;
  }
  #endif

  c->status = napi_set_named_property(env, result, "displayModeName", param);
  REJECT_STATUS;

  int32_t width, height, rowBytes;
  width = c->selectedDisplayMode->GetWidth();
  height = c->selectedDisplayMode->GetHeight();
  switch (c->requestedPixelFormat) {
    case bmdFormat8BitYUV:
      rowBytes = width * 2;
      break;
    case bmdFormat10BitYUV:
      rowBytes = ((int32_t) ((width + 47) / 48)) * 128;
      break;
    case bmdFormat8BitARGB:
    case bmdFormat8BitBGRA:
      rowBytes = width * 4;
      break;
    case bmdFormat10BitRGB:
    case bmdFormat10BitRGBXLE:
    case bmdFormat10BitRGBX:
      rowBytes = ((int32_t) ((width + 63) / 64)) * 256;
      break;
    case bmdFormat12BitRGB:
    case bmdFormat12BitRGBLE:
      rowBytes = (int32_t) ((width * 36) / 8);
      break;
    default:
      rowBytes = -1;
      break;
  }

  c->status = napi_create_int32(env, width, &param);
  REJECT_STATUS;
  c->status = napi_set_named_property(env, result, "width", param);
  REJECT_STATUS;

  c->status = napi_create_int32(env, height, &param);
  REJECT_STATUS;
  c->status = napi_set_named_property(env, result, "height", param);
  REJECT_STATUS;

  c->status = napi_create_int32(env, rowBytes, &param);
  REJECT_STATUS;
  c->status = napi_set_named_property(env, result, "rowBytes", param);
  REJECT_STATUS;

  c->status = napi_create_int32(env, rowBytes * height, &param);
  REJECT_STATUS;
  c->status = napi_set_named_property(env, result, "bufferSize", param);
  REJECT_STATUS;

  switch (c->selectedDisplayMode->GetFieldDominance()) {
    case bmdLowerFieldFirst:
      c->status = napi_create_string_utf8(env, "lowerFieldFirst", NAPI_AUTO_LENGTH, &param);
      break;
    case bmdUpperFieldFirst:
      c->status = napi_create_string_utf8(env, "upperFieldFirst", NAPI_AUTO_LENGTH, &param);
      break;
    case bmdProgressiveFrame:
      c->status = napi_create_string_utf8(env, "progressiveFrame", NAPI_AUTO_LENGTH, &param);
      break;
    default:
      c->status = napi_create_string_utf8(env, "unknown", NAPI_AUTO_LENGTH, &param);
      break;
  }
  REJECT_STATUS;
  c->status = napi_set_named_property(env, result, "fieldDominance", param);
  REJECT_STATUS;

  hresult = c->selectedDisplayMode->GetFrameRate(&frameRateDuration, &frameRateScale);
  if (hresult == S_OK) {
    c->status = napi_create_array(env, &param);
    REJECT_STATUS;
    c->status = napi_create_int64(env, frameRateDuration, &paramPart);
    REJECT_STATUS;
    c->status = napi_set_element(env, param, 0, paramPart);
    REJECT_STATUS;
    c->status = napi_create_int64(env, frameRateScale, &paramPart);
    REJECT_STATUS;
    c->status = napi_set_element(env, param, 1, paramPart);
    REJECT_STATUS;
    c->status = napi_set_named_property(env, result, "frameRate", param);
    REJECT_STATUS;
  }

  uint32_t pixelFormatIndex = 0;

  while ((gKnownPixelFormats[pixelFormatIndex] != 0) &&
      (gKnownPixelFormatNames[pixelFormatIndex] != NULL)) {
    if (c->requestedPixelFormat == gKnownPixelFormats[pixelFormatIndex]) {
      c->status = napi_create_string_utf8(env, gKnownPixelFormatNames[pixelFormatIndex],
        NAPI_AUTO_LENGTH, &param);
      REJECT_STATUS;
      c->status = napi_set_named_property(env, result, "pixelFormat", param);
      REJECT_STATUS;
      break;
    }
    pixelFormatIndex++;
  }

  c->status = napi_get_boolean(env, (c->channels > 0), &param);
  REJECT_STATUS;
  c->status = napi_set_named_property(env, result, "audioEnabled", param);
  REJECT_STATUS;

  if (c->channels > 0) {
    c->status = napi_create_int32(env, c->requestedSampleRate, &param);
    REJECT_STATUS;
    c->status = napi_set_named_property(env, result, "sampleRate", param);
    REJECT_STATUS;

    c->status = napi_create_int32(env, c->requestedSampleType, &param);
    REJECT_STATUS;
    c->status = napi_set_named_property(env, result, "sampleType", param);
    REJECT_STATUS;

    c->status = napi_create_int32(env, c->channels, &param);
    REJECT_STATUS;
    c->status = napi_set_named_property(env, result, "channels", param);
    REJECT_STATUS;
  };

  playbackThreadsafe* pbts = new playbackThreadsafe;
  pbts->deckLinkOutput = c->deckLinkOutput;
  c->deckLinkOutput = nullptr;
  pbts->displayMode = c->selectedDisplayMode;
  c->selectedDisplayMode = nullptr;
  pbts->timeScale = frameRateScale;
  pbts->width = width;
  pbts->height = height;
  pbts->rowBytes = rowBytes;
  pbts->pixelFormat = c->requestedPixelFormat;
  pbts->channels = c->channels;
  if (c->channels > 0) {
    pbts->sampleRate = c->requestedSampleRate;
    pbts->sampleType = c->requestedSampleType;
  }

  hresult = pbts->deckLinkOutput->SetScheduledFrameCompletionCallback(pbts);
  if (hresult != S_OK) {
    c->status = MACADAM_CALL_FAILURE;
    c->errorMsg = "Unable to set callback for deck link output.";
    REJECT_STATUS;
  }

  c->status = napi_create_function(env, "displayFrame", NAPI_AUTO_LENGTH, displayFrame,
    nullptr, &param);
  REJECT_STATUS;
  c->status = napi_set_named_property(env, result, "displayFrame", param);
  REJECT_STATUS;

  c->status = napi_create_function(env, "stop", NAPI_AUTO_LENGTH, stopPlayback,
    nullptr, &param);
  REJECT_STATUS;
  c->status = napi_set_named_property(env, result, "stop", param);
  REJECT_STATUS;

  c->status = napi_create_string_utf8(env, "playback", NAPI_AUTO_LENGTH, &asyncName);
  REJECT_STATUS;
  c->status = napi_create_function(env, "nop", NAPI_AUTO_LENGTH, nop, nullptr, &param);
  REJECT_STATUS;
  c->status = napi_create_threadsafe_function(env, param, nullptr, asyncName,
    20, 1, nullptr, playbackTsFnFinalize, pbts, playedFrame, &pbts->tsFn);
  REJECT_STATUS;

  c->status = napi_create_external(env, pbts, finalizePlaybackCarrier, nullptr, &param);
  REJECT_STATUS;
  c->status = napi_set_named_property(env, result, "deckLinkOutput", param);
  REJECT_STATUS;

  napi_status status;
  status = napi_resolve_deferred(env, c->_deferred, result);
  FLOATING_STATUS;

  tidyCarrier(env, c);
}

napi_value playback(napi_env env, napi_callback_info info) {
  napi_value promise, resourceName, options, param;
  napi_valuetype type;
  bool isArray;
  playbackCarrier* c = new playbackCarrier;

  c->status = napi_create_promise(env, &c->_deferred, &promise);
  REJECT_RETURN;

  c->requestedDisplayMode = bmdModeHD1080i50;
  c->requestedPixelFormat = bmdFormat10BitYUV;
  size_t argc = 1;
  napi_value args[1];
  c->status = napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  REJECT_RETURN;

  if (argc >= 1) {
    c->status = napi_typeof(env, args[0], &type);
    REJECT_RETURN;
    c->status = napi_is_array(env, args[0], &isArray);
    REJECT_RETURN;
    if ((type != napi_object) || (isArray == true)) REJECT_ERROR_RETURN(
        "Options provided to capture create must be an object and not an array.",
        MACADAM_INVALID_ARGS);
    options = args[0];
  }
  else {
    c->status = napi_create_object(env, &options);
    REJECT_RETURN;
  }

  c->status = napi_get_named_property(env, options, "deviceIndex", &param);
  REJECT_RETURN;
  c->status = napi_typeof(env, param, &type);
  REJECT_RETURN;
  if (type != napi_undefined) {
    if (type != napi_number) REJECT_ERROR_RETURN(
      "Device index must be a number.", MACADAM_INVALID_ARGS);
    c->status = napi_get_value_uint32(env, param, &c->deviceIndex);
    REJECT_RETURN;
  }

  c->status = napi_get_named_property(env, options, "displayMode", &param);
  REJECT_RETURN;
  c->status = napi_typeof(env, param, &type);
  REJECT_RETURN;
  if (type != napi_undefined) {
    if (type != napi_number) REJECT_ERROR_RETURN(
      "Display mode must be an enumeration value.", MACADAM_INVALID_ARGS);
    c->status = napi_get_value_uint32(env, param, (uint32_t *) &c->requestedDisplayMode);
    REJECT_RETURN;
  }

  c->status = napi_get_named_property(env, options, "pixelFormat", &param);
  REJECT_RETURN;
  c->status = napi_typeof(env, param, &type);
  REJECT_RETURN;
  if (type != napi_undefined) {
    if (type != napi_number) REJECT_ERROR_RETURN(
      "Pixel format must be an enumeration value.", MACADAM_INVALID_ARGS);
    c->status = napi_get_value_uint32(env, param, (uint32_t *) &c->requestedPixelFormat);
    REJECT_RETURN;
  }

  c->status = napi_get_named_property(env, options, "channels", &param);
  REJECT_RETURN;
  c->status = napi_typeof(env, param, &type);
  REJECT_RETURN;
  if (type != napi_undefined) {
    if (type != napi_number) REJECT_ERROR_RETURN(
      "Audio channel count must be a number.", MACADAM_INVALID_ARGS);
    c->status = napi_get_value_uint32(env, param, &c->channels);
    REJECT_RETURN;
  }

  c->status = napi_get_named_property(env, options, "sampleRate", &param);
  REJECT_RETURN;
  c->status = napi_typeof(env, param, &type);
  REJECT_RETURN;
  if (type != napi_undefined) {
    if (type != napi_number) REJECT_ERROR_RETURN(
      "Audio sample rate must be an enumeration value.", MACADAM_INVALID_ARGS);
    c->status = napi_get_value_uint32(env, param, (uint32_t *) &c->requestedSampleRate);
    REJECT_RETURN;
  }

  c->status = napi_get_named_property(env, options, "sampleType", &param);
  REJECT_RETURN;
  c->status = napi_typeof(env, param, &type);
  REJECT_RETURN;
  if (type != napi_undefined) {
    if (type != napi_number) REJECT_ERROR_RETURN(
      "Audio sample type must be an enumeration value.", MACADAM_INVALID_ARGS);
    c->status = napi_get_value_uint32(env, param, (uint32_t *) &c->requestedSampleType);
    REJECT_RETURN;
  }

  c->status = napi_create_string_utf8(env, "CreatePlayback", NAPI_AUTO_LENGTH, &resourceName);
  REJECT_RETURN;
  c->status = napi_create_async_work(env, NULL, resourceName, playbackExecute,
    playbackComplete, c, &c->_request);
  REJECT_RETURN;
  c->status = napi_queue_async_work(env, c->_request);
  REJECT_RETURN;

  return promise;
}

void playedFrame(napi_env env, napi_value jsCb, void* context, void* data) {

  return;
}

void displayFrameExecute(napi_env env, void* data) {
  displayFrameCarrier* c = (displayFrameCarrier*) data;
  HRESULT hresult;

  void* testBytes;
  c->GetBytes(&testBytes);
  printf("Test bytes %p and pixel format %i=%i.\n", testBytes, c->GetPixelFormat(), bmdFormat10BitYUV);

  /* IDeckLinkMutableVideoFrame* frame;
  hresult = c->deckLinkOutput->CreateVideoFrame(c->width, c->height, c->rowBytes,
    c->pixelFormat, bmdFrameFlagDefault, &frame);
  if (hresult != S_OK) {
    printf("Problem creating frame.\n");
  }
  void* frameBytes;
  frame->GetBytes(&frameBytes);
  memcpy(frameBytes, c->data, c->dataSize); */

  printf("Some data %02x %02x %02x %02x\n", ((uint8_t*) testBytes)[0],
    ((uint8_t*) testBytes)[1], ((uint8_t*) testBytes)[2], ((uint8_t*) testBytes)[3]);

  // This call may block - make sure thread pool is large enough
  hresult = c->deckLinkOutput->DisplayVideoFrameSync(c);
  switch (hresult) {
    case E_FAIL:
      c->status = MACADAM_CALL_FAILURE;
      c->errorMsg = "Failed to display a video frame.";
      break;
    case E_ACCESSDENIED:
      c->status = MACADAM_ACCESS_DENIED;
      c->errorMsg = "On request to display a frame, the video output is not enabled.";
      break;
    case E_INVALIDARG:
      c->status = MACADAM_INVALID_ARGS;
      c->errorMsg = "On request to display a frame, the frame attributes are not valid.";
      break;
    case S_OK:
      break;
    default:
      break;
  }
  // frame->Release();
}

void displayFrameComplete(napi_env env, napi_status asyncStatus, void* data) {
  displayFrameCarrier* c = (displayFrameCarrier*) data;
  napi_value result;

  if (asyncStatus != napi_ok) {
    c->status = asyncStatus;
    c->errorMsg = "Display frame failed to complete.";
  }
  REJECT_STATUS;

  c->status = napi_create_object(env, &result);
  REJECT_STATUS;

  napi_status status;
  status = napi_resolve_deferred(env, c->_deferred, result);
  FLOATING_STATUS;

  tidyCarrier(env, c);
}

napi_value displayFrame(napi_env env, napi_callback_info info) {
  napi_value promise, resourceName, playback, param;
  playbackThreadsafe* pbts;
  displayFrameCarrier* c = new displayFrameCarrier;
  bool isBuffer;

  c->status = napi_create_promise(env, &c->_deferred, &promise);
  REJECT_RETURN;

  size_t argc = 1;
  napi_value argv[1];
  c->status = napi_get_cb_info(env, info, &argc, argv, &playback, nullptr);
  REJECT_RETURN;

  if (argc != 1) REJECT_ERROR_RETURN(
    "Frame can only be displayed from a buffer of data.", MACADAM_INVALID_ARGS);

  c->status = napi_is_buffer(env, argv[0], &isBuffer);
  REJECT_RETURN;

  if (!isBuffer) REJECT_ERROR_RETURN(
    "Frame data must be provided as a node buffer.", MACADAM_INVALID_ARGS);

  c->status = napi_get_buffer_info(env, argv[0], &c->data, &c->dataSize);
  REJECT_RETURN;

  c->status = napi_get_named_property(env, playback, "deckLinkOutput", &param);
  REJECT_RETURN;
  c->status = napi_get_value_external(env, param, (void**) &pbts);
  REJECT_RETURN;

  /* if (pbts->started) REJECT_ERROR_RETURN(
    "Display frame cannot be used in conjuction with scheduled playback.",
    MACADAM_ERROR_START); */
  /* if (!pbts->started) {
    c->deckLinkOutput->StartScheduledPlayback(0, 1000, 1.0);
    pbts->started = true;
  } */

  if (pbts->stopped) REJECT_ERROR_RETURN(
    "Display frame cannot be used once an output is stopped.",
    MACADAM_ERROR_START);

  c->width = pbts->width;
  c->height = pbts->height;
  c->rowBytes = pbts->rowBytes;
  c->pixelFormat = pbts->pixelFormat;
  c->deckLinkOutput = pbts->deckLinkOutput;

  if (((int32_t) c->dataSize) < c->rowBytes * c->height) REJECT_ERROR_RETURN(
    "Insufficient number of bytes in buffer for frame playback.",
    MACADAM_INSUFFICIENT_BYTES);

  c->status = napi_create_reference(env, argv[0], 1, &c->passthru);
  REJECT_RETURN;

  c->status = napi_create_string_utf8(env, "DisplayFrame", NAPI_AUTO_LENGTH, &resourceName);
  REJECT_RETURN;
  c->status = napi_create_async_work(env, nullptr, resourceName, displayFrameExecute,
    displayFrameComplete, c, &c->_request);
  REJECT_RETURN;
  c->status = napi_queue_async_work(env, c->_request);
  REJECT_RETURN;

  return promise;
}

napi_value stopPlayback(napi_env env, napi_callback_info info) {
  napi_status status;
  napi_value playback, param, value;
  playbackThreadsafe* pbts;
  HRESULT hresult;

  size_t argc = 0;
  status = napi_get_cb_info(env, info, &argc, nullptr, &playback, nullptr);
  CHECK_STATUS;

  status = napi_get_named_property(env, playback, "deckLinkOutput", &param);
  CHECK_STATUS;
  status = napi_get_value_external(env, param, (void**) &pbts);
  CHECK_STATUS;

  if (pbts->stopped) NAPI_THROW_ERROR("Already stopped.");

  if (pbts->started) {
    hresult = pbts->deckLinkOutput->StopScheduledPlayback(0, nullptr, 0);
    if (hresult != S_OK) NAPI_THROW_ERROR("Failed to stop scheduled playback.");
  }

  hresult = pbts->deckLinkOutput->DisableVideoOutput();
  if (hresult != S_OK) NAPI_THROW_ERROR("Failed to disable video output.");

  hresult = pbts->deckLinkOutput->SetScheduledFrameCompletionCallback(nullptr);
  if (hresult != S_OK) NAPI_THROW_ERROR("Failed to clear the frame completion callback.");

  if (pbts->channels > 0) {
    hresult = pbts->deckLinkOutput->DisableAudioOutput();
    if (hresult != S_OK) NAPI_THROW_ERROR("Failed to disable audio output.");
  }

  // TODO consider clearing audio callback as a matter of course

  status = napi_release_threadsafe_function(pbts->tsFn, napi_tsfn_release);
  CHECK_STATUS;
  pbts->stopped = true;

  status = napi_get_undefined(env, &value);
  CHECK_STATUS;
  return value;
}

void playbackTsFnFinalize(napi_env env, void* data, void* hint) {
  printf("Threadsafe playback finalizer called.\n");
  // FIXME: Implement this
}
