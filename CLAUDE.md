# HandSight - Android-to-Windows Live Camera Stream

A real-time camera streaming application that transmits JPEG frames from an Android phone to a Windows receiver app over a network connection using the `HandSightStreamProtocol`.

## Project Structure

```
HandSight/
├── Hand-Sight/                  # Windows receiver app (C++)
│   ├── Hand-Sight.cpp          # Main Windows UI and networking
│   ├── Hand-Sight.h            # Header
│   ├── CMakeLists.txt          # CMake build configuration
│   └── out/                    # Build output directory
│
├── android/HandSightCamera/    # Android sender app (NDK)
│   ├── jni/
│   │   └── HandSightCamera.cpp # Android native camera code
│   ├── AndroidManifest.xml     # Android app manifest
│   ├── Application.mk          # NDK build config
│   ├── Android.mk              # NDK build rules
│   ├── build.ps1               # PowerShell build script
│   └── out/                    # Build output (APKs)
│
└── shared/
    └── HandSightStreamProtocol.h  # Network protocol header (shared between apps)
```

## Technology Stack

- **Windows App**: C++20, WinSock2, Windows GDI, Windows Imaging Component (WIC)
- **Android App**: Android NDK, Camera2 NDK API, JPEG encoding
- **Protocol**: Custom HSF2 (HandSight Format v2) over TCP with JPEG frames
- **Build**: CMake (Windows), Android NDK (Android)

## Setup and Build

### Windows Requirements
- CMake 3.10+
- LLVM/Clang C++ compiler
- Windows 10+

### Build Windows App
```bash
cd Hand-Sight
mkdir out && cd out
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . --config Debug
# Output: Hand-Sight\out\Hand-Sight.exe
```

### Build Android App
```bash
cd android/HandSightCamera
./build.ps1  # Or use Android Studio
# Output: HandSightCamera/out/HandSightCamera-signed.apk
```

## Running the Application

### Prerequisites
1. **Windows PC** with the built Hand-Sight.exe
2. **Android Phone** with HandSightCamera app installed
3. **USB Connection** with ADB enabled

### Launch Steps

1. **Start Windows app** (listens on port 5001):
   ```
   Hand-Sight\out\Hand-Sight.exe
   ```
   Window will show: "Waiting for adb reverse client on 127.0.0.1:5001..."

2. **Set up ADB reverse tunnel** (on Windows command line):
   ```bash
   adb reverse tcp:5001 tcp:5001
   ```
   This forwards the phone's port 5001 to the PC's port 5001.

3. **Start Android app**:
   - Tap the app icon on your phone, or via ADB:
   ```bash
   adb shell am start -n "com.handsight.camera/android.app.NativeActivity"
   ```

4. **Verify Connection**:
   - Windows app should show: "Client connected. Receiving JPEG frames..."
   - Camera feed should appear in the Windows window
   - Phone camera should be streaming at 1280x960 JPEG @ ~30fps

### Status Messages

| Message | Meaning | Action |
|---------|---------|--------|
| "Waiting for adb reverse client..." | Ready but no phone connected | Run `adb reverse tcp:5001 tcp:5001` |
| "Client connected. Receiving JPEG frames..." | ✅ Streaming active | Video should display |
| "Connection lost. Waiting for reconnect..." | Network dropped | Restart Android app |
| "JPEG decode failed" | Frame corruption | Check network quality |

## Key Bug Fixes Applied

### Image Scaling Issue (Fixed 2026-05-22)
- **Problem**: Video appeared stretched/distorted
- **Cause**: ComputeFitRect() used `std::max()` instead of `std::min()` for scale calculation, causing oversized rendering
- **Fix**: Changed to proper aspect-ratio-preserving scaling with `std::min()` and removed forced minimum size constraints
- **Lines**: Hand-Sight.cpp:130-133

## Network Protocol

**StreamHeader** (sent once at connection):
- Magic: 0x48534632 ("HSF2")
- Version: 2
- Width, Height: Frame dimensions
- PixelFormat: 1 (JPEG)
- RotationDegrees: 0, 90, 180, or 270

**FrameHeader** (sent per frame):
- PayloadSize: JPEG data size
- TimestampUs: Frame timestamp in microseconds

**Payload**: Raw JPEG data (typically 27-140KB per frame at quality 92)

## Troubleshooting

### Windows app shows blank screen
- Check: Is the status message visible? (should show connection status)
- Solution: Verify adb reverse is active: `adb reverse --list`

### Phone app won't connect
- Check: `adb devices` shows your phone
- Check: `adb logcat -s HandSightCamera` for errors
- Solution: Restart app or reconnect phone

### Low frame rate
- Check network quality and bandwidth
- Reduce frame payload size or resolution on Android side
- JPEG quality is set to 92 (kJpegQuality in HandSightCamera.cpp:42)

### Camera permission denied
- On Android: Grant camera permission in app settings
- Phone logs will show "Camera permission granted" when ready

## Development Notes

- **Thread Model**: Network I/O in separate thread, UI updates via Windows message loop
- **Synchronization**: Mutex-protected frame buffer between threads
- **Android**: Uses native camera API, continuous preview mode with auto exposure/focus
- **Frame Queue**: Only keeps latest frame (buffer.clear() at line 759)
- **Zoom**: Attempts 0.6x zoom if device supports it, otherwise uses full sensor

## Next Steps / Known Improvements

- [ ] Add frame rate statistics display
- [ ] Support multiple resolution options
- [ ] Add on-screen overlay for connection info
- [ ] Implement graceful reconnection with exponential backoff
- [ ] Cross-platform build (Android studio integration)
- [ ] Audio stream support

## Team / Contact

Created: 2026-05-22
Last Modified: 2026-05-22
