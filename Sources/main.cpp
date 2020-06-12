#include <StreamDeckSDK/ESDLogger.h>
#include <StreamDeckSDK/ESDMain.h>

#include "ZoomStreamDeckPlugin.h"

int main(int argc, const char** argv) {
  ESDLogger::Get()->SetWin32DebugPrefix("[SDZoom] ");
  return esd_main(argc, argv, new ZoomStreamDeckPlugin());
}
