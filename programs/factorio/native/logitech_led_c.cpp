// The functions of the Logitech LED SDK the game calls (src/LED/Logitech), as C functions for kpp:
// LogitechLEDLib.lib (libraries/Logitech/x64, on the game's link line) is C++, so its functions have
// C++'s names. overrides/src/LED/Logitech/Includes/LogitechLEDLib.kpp defines the header's
// functions on these. The enums cross as their int values.
#include <LED/Logitech/Includes/LogitechLEDLib.h>

extern "C"
{
  bool kppLogiLedInitWithName(const char* name) { return LogiLedInitWithName(name); }
  bool kppLogiLedSetTargetDevice(int targetDevice) { return LogiLedSetTargetDevice(targetDevice); }
  bool kppLogiLedSetLightingFromBitmap(unsigned char* bitmap) { return LogiLedSetLightingFromBitmap(bitmap); }
  bool kppLogiLedSetLightingForKeyWithKeyName(int keyName, int redPercentage, int greenPercentage, int bluePercentage)
  {
    return LogiLedSetLightingForKeyWithKeyName(LogiLed::KeyName(keyName), redPercentage, greenPercentage, bluePercentage);
  }
  bool kppLogiLedSetLightingForTargetZone(int deviceType, int zone, int redPercentage, int greenPercentage, int bluePercentage)
  {
    return LogiLedSetLightingForTargetZone(LogiLed::DeviceType(deviceType), zone, redPercentage, greenPercentage, bluePercentage);
  }
  void kppLogiLedShutdown() { LogiLedShutdown(); }
}
