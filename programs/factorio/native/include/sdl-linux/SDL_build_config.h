/* SDL3's configuration for the kpp game's Linux build: Factorio's own (libraries/SDL3/build_config_linux)
   without what Debian 11's headers (the glibc 2.31 sysroot) cannot build: Wayland, whose protocol code
   Factorio's build generates with wayland-scanner (none on Windows); PipeWire, whose 0.3.19 headers lack
   what SDL3 uses (PW_KEY_CONFIG_NAME, SPA_PTROFF); and XInput2's gestures, which libXi 1.7.10 has not
   (XI_GesturePinchBegin). SDL picks X11, and PulseAudio or ALSA, at run time as before. */
#include "build_config_linux/SDL_build_config.h" /* (libraries/SDL3 on the include path) */
#undef SDL_VIDEO_DRIVER_WAYLAND
#undef SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC
#undef SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_CURSOR
#undef SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_EGL
#undef SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR
#undef SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_XKBCOMMON
#undef SDL_AUDIO_DRIVER_PIPEWIRE
#undef SDL_AUDIO_DRIVER_PIPEWIRE_DYNAMIC
#undef SDL_CAMERA_DRIVER_PIPEWIRE
#undef SDL_CAMERA_DRIVER_PIPEWIRE_DYNAMIC
#undef SDL_VIDEO_DRIVER_X11_XINPUT2_SUPPORTS_GESTURE
