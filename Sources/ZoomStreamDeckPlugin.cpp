#include "ZoomStreamDeckPlugin.h"

#include <StreamDeckSDK/EPLJSONUtils.h>
#include <StreamDeckSDK/ESDConnectionManager.h>
#include <StreamDeckSDK/ESDLogger.h>

#include <atomic>
#include <iostream>
#include <mutex>
#include <vector>

#define MUTETOGGLE_ACTION_ID "com.lostdomain.zoom.mutetoggle"
#define VIDEOTOGGLE_ACTION_ID "com.lostdomain.zoom.videotoggle"
#define SHARETOGGLE_ACTION_ID "com.lostdomain.zoom.sharetoggle"
#define FOCUS_ACTION_ID "com.lostdomain.zoom.focus"
#define LEAVE_ACTION_ID "com.lostdomain.zoom.leave"

class CallBackTimer {
 public:
  CallBackTimer() : _execute(false) {
  }
  ~CallBackTimer() {
    if (_execute.load(std::memory_order_acquire)) {
      stop();
    };
  }
  void stop() {
    _execute.store(false, std::memory_order_release);
    if (_thd.joinable())
      _thd.join();
  }
  void start(int interval, std::function<void(void)> func) {
    if (_execute.load(std::memory_order_acquire)) {
      stop();
    };
    _execute.store(true, std::memory_order_release);
    _thd = std::thread([this, interval, func]() {
      while (_execute.load(std::memory_order_acquire)) {
        func();
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
      }
    });
  }
  bool is_running() const noexcept {
    return (_execute.load(std::memory_order_acquire) && _thd.joinable());
  }

 private:
  std::atomic<bool> _execute;
  std::thread _thd;
};

char* execAndReturn(const char* command) {
  FILE* fp;
  char* line = NULL;
  // Following initialization is equivalent to char* result = ""; and just
  // initializes result to an empty string, only it works with
  // -Werror=write-strings and is so much less clear.
  char* result = (char*)calloc(1, 1);
  size_t len = 0;

  fflush(NULL);
  fp = popen(command, "r");
  if (fp == NULL) {
    printf("Cannot execute command:\n%s\n", command);
    return NULL;
  }

  while (getline(&line, &len, fp) != -1) {
    // +1 below to allow room for null terminator.
    result = (char*)realloc(result, strlen(result) + strlen(line) + 1);
    // +1 below so we copy the final null terminator.
    strncpy(result + strlen(result), line, strlen(line) + 1);
    free(line);
    line = NULL;
  }

  fflush(fp);
  if (pclose(fp) != 0) {
    perror("Cannot close stream.\n");
  }
  return result;
}

json getZoomStatus() {
  // get Zoom Mute status
  // ESDDebug("APPLESCRIPT_GET_STATUS: %s", APPLESCRIPT_GET_STATUS);
  char* zoomStatus = execAndReturn(
    "osascript -e 'set zoomStatus to \"closed\"\nset muteStatus to "
    "\"muted\"\nset videoStatus to \"stopped\"\nset shareStatus to "
    "\"stopped\"\ntell application \"System Events\"\nif exists (window 1 of "
    "process \"zoom.us\") then\nset zoomStatus to \"open\"\nend if\nend "
    "tell\nif (zoomStatus contains \"open\") then\ntell application \"System "
    "Events\" to tell application process \"zoom.us\"\nif exists (menu item "
    "\"Mute audio\" of menu 1 of menu bar item \"Meeting\" of menu bar 1) "
    "then\nset muteStatus to \"unmuted\"\nelse\nset muteStatus to "
    "\"muted\"\nend if\nif exists (menu item \"Start Video\" of menu 1 of menu "
    "bar item \"Meeting\" of menu bar 1) then\nset videoStatus to "
    "\"stopped\"\nelse\nset videoStatus to \"started\"\nend if\nif exists "
    "(menu item \"Start Share\" of menu 1 of menu bar item \"Meeting\" of menu "
    "bar 1) then\nset shareStatus to \"stopped\"\nelse\nset shareStatus to "
    "\"started\"\nend if\nend tell\nend if\ndo shell script \"echo mute:\" & "
    "(muteStatus as text) & \",video:\" & (videoStatus as text) & \",zoom:\" & "
    "(zoomStatus as text) & \",share:\" & (shareStatus as text)'");
  std::string status = std::string(zoomStatus);

  std::string statusMute;
  std::string statusVideo;
  std::string statusShare;
  std::string statusZoom;

  if (status.find("zoom:open") != std::string::npos) {
    ESDDebug("Zoom Open!");
    statusZoom = "open";
  } else {
    ESDDebug("Zoom Closed!");
    statusZoom = "closed";
  }

  if (status.find("mute:muted") != std::string::npos) {
    ESDDebug("Zoom Muted!");
    statusMute = "muted";
  } else {
    ESDDebug("Zoom Unmuted!");
    statusMute = "unmuted";
  }

  if (status.find("video:started") != std::string::npos) {
    ESDDebug("Zoom Video Started!");
    statusVideo = "started";
  } else {
    ESDDebug("Zoom Video Stopped!");
    statusVideo = "stopped";
  }

  if (status.find("share:started") != std::string::npos) {
    ESDDebug("Zoom Screen Sharing Started!");
    statusShare = "started";
  } else {
    ESDDebug("Zoom Screen Sharing Stopped!");
    statusShare = "stopped";
  }

  ESDDebug("Zoom status: %s", zoomStatus);

  return json({{"statusZoom", statusZoom},
               {"statusMute", statusMute},
               {"statusVideo", statusVideo},
               {"statusShare", statusShare}});
}

ZoomStreamDeckPlugin::ZoomStreamDeckPlugin() {
  ESDDebug("stored handle");

  // start a timer that updates the current status every 3 seconds
  mTimer = new CallBackTimer();
  mTimer->start(3000, [this]() { this->UpdateZoomStatus(); });
}

ZoomStreamDeckPlugin::~ZoomStreamDeckPlugin() {
  ESDDebug("plugin destructor");
}

void ZoomStreamDeckPlugin::UpdateZoomStatus() {
  // This is running in a different thread
  if (mConnectionManager != nullptr) {
    std::scoped_lock lock(mVisibleContextsMutex);

    // ESDDebug("UpdateZoomStatus");
    // get zoom status for mute, video and whether it's open
    json newStatus = getZoomStatus();
    auto newMuteState = 0;
    auto newVideoState = 0;
    auto newShareState = 0;

    if (EPLJSONUtils::GetStringByName(newStatus, "statusMute") == "muted") {
      // ESDDebug("CURRENT: Zoom muted");
      newMuteState = 0;
    } else {
      // ESDDebug("CURRENT: Zoom unmuted");
      newMuteState = 1;
    }

    if (EPLJSONUtils::GetStringByName(newStatus, "statusVideo") == "stopped") {
      // ESDDebug("CURRENT: Zoom video stopped");
      newVideoState = 0;
    } else {
      // ESDDebug("CURRENT: Zoom video started");
      newVideoState = 1;
    }

    if (EPLJSONUtils::GetStringByName(newStatus, "statusShare") == "stopped") {
      newShareState = 0;
    } else {
      newShareState = 1;
    }

    // sanity check
    if (mButtons.count(MUTETOGGLE_ACTION_ID)) {
      // update mute button
      const auto button = mButtons[MUTETOGGLE_ACTION_ID];
      // ESDDebug("Mute button context: %s", button.context.c_str());
      mConnectionManager->SetState(newMuteState, button.context);
    }

    // sanity check
    if (mButtons.count(VIDEOTOGGLE_ACTION_ID)) {
      // update video button
      const auto button = mButtons[VIDEOTOGGLE_ACTION_ID];
      // ESDDebug("Video button context: %s", button.context.c_str());
      mConnectionManager->SetState(newVideoState, button.context);
    }

    // sanity check
    if (mButtons.count(SHARETOGGLE_ACTION_ID)) {
      // update video button
      const auto button = mButtons[SHARETOGGLE_ACTION_ID];
      // ESDDebug("Video button context: %s", button.context.c_str());
      mConnectionManager->SetState(newShareState, button.context);
    }
  }
}

void ZoomStreamDeckPlugin::KeyDownForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  const auto state = EPLJSONUtils::GetIntByName(inPayload, "state");
}

void ZoomStreamDeckPlugin::KeyUpForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  ESDDebug("Key Up: %s", inPayload.dump().c_str());
  std::scoped_lock lock(mVisibleContextsMutex);

  const auto state = EPLJSONUtils::GetIntByName(inPayload, "state");
  bool updateStatus = false;
  auto newState = 0;

  if (inAction == MUTETOGGLE_ACTION_ID) {
    // state == 0 == want to be muted
    if (state != 0) {
      ESDDebug("Unmuting Zoom!");
    }
    // state == 1 == want to be unmuted
    else {
      ESDDebug("Muting Zoom!");
    }

    const char* script
      = "osascript -e 'tell application \"zoom.us\"\ntell application \"System "
        "Events\"\nkeystroke \"a\" using {shift down, command down}\nend "
        "tell\nend tell'";
    system(script);
    updateStatus = true;
  } else if (inAction == SHARETOGGLE_ACTION_ID) {
    // state == 0 == want to share
    if (state != 0) {
      ESDDebug("Sharing Screen on Zoom!");
    }
    // state == 1 == want to stop sharing
    else {
      ESDDebug("Stopping Screen Sharing on Zoom!");
    }

    const char* script
      = "osascript -e 'tell application \"zoom.us\"\ntell application \"System "
        "Events\"\nkeystroke \"s\" using {shift down, command down}\nend "
        "tell\nend tell'";
    system(script);
    updateStatus = true;
  } else if (inAction == VIDEOTOGGLE_ACTION_ID) {
    // state == 0 == want to be with video on
    if (state != 0) {
      ESDDebug("Starting Zoom Video!");
    }
    // state == 1 == want to be with video off
    else {
      ESDDebug("Stopping Zoom Video!");
    }

    const char* script
      = "osascript -e 'tell application \"zoom.us\"\ntell application \"System "
        "Events\"\nkeystroke \"v\" using {shift down, command down}\nend "
        "tell\nend tell'";
    system(script);
    updateStatus = true;
  }
  // focus on Zoom window
  else if (inAction == FOCUS_ACTION_ID) {
    const char* script
      = "osascript -e 'tell application \"zoom.us\"\nactivate\nend tell'";
    system(script);
  }
  // leave Zoom meeting, or end the meeting. When ending, this also clicks "End
  // for all"
  else if (inAction == LEAVE_ACTION_ID) {
    const char* script
      = "osascript -e 'tell application \"zoom.us\"\nactivate\ntell "
        "application \"System Events\"\nkeystroke \"w\" using {command "
        "down}\nend tell\ntell application \"System Events\"\ntell front "
        "window of (first application process whose frontmost is true)\nclick "
        "button 1\nend tell\nend tell\nend tell\n'";
    system(script);
  }

  if (updateStatus) {
    // make sure we're synchronised to the actual status by requesting the
    // current status
    json newStatus = getZoomStatus();

    if (inAction == MUTETOGGLE_ACTION_ID) {
      if (EPLJSONUtils::GetStringByName(newStatus, "statusMute") == "muted") {
        ESDDebug("CURRENT: Zoom muted!");
        newState = 0;
      } else {
        ESDDebug("CURRENT: Zoom unmuted!");
        newState = 1;
      }
    } else if (inAction == VIDEOTOGGLE_ACTION_ID) {
      if (
        EPLJSONUtils::GetStringByName(newStatus, "statusVideo") == "stopped") {
        ESDDebug("CURRENT: Zoom video stopped!");
        newState = 0;
      } else {
        ESDDebug("CURRENT: Zoom video started!");
        newState = 1;
      }
    }

    else if (inAction == SHARETOGGLE_ACTION_ID) {
      if (
        EPLJSONUtils::GetStringByName(newStatus, "statusShare") == "stopped") {
        ESDDebug("CURRENT: Zoom screen sharing stopped!");
        newState = 0;
      } else {
        ESDDebug("CURRENT: Zoom screen sharing started!");
        newState = 1;
      }
    }

    mConnectionManager->SetState(newState, inContext);
  }
}

void ZoomStreamDeckPlugin::WillAppearForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  std::scoped_lock lock(mVisibleContextsMutex);
  // Remember the button context for the timer updates
  mVisibleContexts.insert(inContext);
  // ESDDebug("Will appear: %s %s", inAction, inContext);
  mButtons[inAction] = {inAction, inContext};
}

void ZoomStreamDeckPlugin::WillDisappearForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  // Remove the context
  std::scoped_lock lock(mVisibleContextsMutex);
  mVisibleContexts.erase(inContext);
  mButtons.erase(inAction);
}

void ZoomStreamDeckPlugin::SendToPlugin(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  json outPayload;

  const auto event = EPLJSONUtils::GetStringByName(inPayload, "event");
  ESDDebug("Received event %s", event.c_str());

  if (event == "getDeviceList") {
    mConnectionManager->SendToPropertyInspector(
      inAction, inContext,
      json(
        {{"event", event}, {"zoomStatus", "open"}, {"muteStatus", "mutes"}}));
    return;
  }
}

void ZoomStreamDeckPlugin::DeviceDidConnect(
  const std::string& inDeviceID,
  const json& inDeviceInfo) {
  // Nothing to do
}

void ZoomStreamDeckPlugin::DeviceDidDisconnect(const std::string& inDeviceID) {
  // Nothing to do
}

void ZoomStreamDeckPlugin::DidReceiveGlobalSettings(const json& inPayload) {
}

void ZoomStreamDeckPlugin::DidReceiveSettings(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  WillAppearForAction(inAction, inContext, inPayload, inDeviceID);
}
