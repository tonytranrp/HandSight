# HandSight - Android-to-Windows Live Camera Stream

A real-time camera streaming application that transmits JPEG frames from an Android phone to a Windows receiver app over a network connection using the `HandSightStreamProtocol`. Includes comprehensive debug logging and statistics for testing and development.

## Project Structure

```
HandSight/
├── Data-Testing/                 # Core data gathering and testing application
│   ├── Hand-Sight/              # Windows receiver app (C++)
│   │   ├── Hand-Sight.cpp       # Main Windows UI and networking
│   │   ├── Hand-Sight.h         # Header
│   │   ├── CMakeLists.txt       # CMake build configuration
│   │   └── out/                 # Build output directory
│   │
│   ├── android/HandSightCamera/ # Android sender app (NDK) with debug UI
│   │   ├── jni/
│   │   │   └── HandSightCamera.cpp # Android native camera + debug logging
│   │   ├── AndroidManifest.xml  # Android app manifest
│   │   ├── Application.mk       # NDK build config
│   │   ├── Android.mk           # NDK build rules (updated paths)
│   │   ├── build.ps1            # PowerShell build script
│   │   └── out/                 # Build output (APKs)
│   │
│   └── shared/
│       └── HandSightStreamProtocol.h  # Network protocol header
│
├── CLAUDE.md                    # This documentation
├── README.md                    # Project overview
└── LICENSE                      # License
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
cd Data-Testing/Hand-Sight
mkdir out && cd out
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . --config Debug
# Output: Data-Testing/Hand-Sight/out/Hand-Sight.exe
```

### Build Android App
```bash
cd Data-Testing/android/HandSightCamera
./build.ps1  # Or use Android Studio
# Output: Data-Testing/android/HandSightCamera/out/HandSightCamera-signed.apk
```

## Running the Application

### Prerequisites
1. **Windows PC** with the built Hand-Sight.exe
2. **Android Phone** with HandSightCamera app installed
3. **USB Connection** with ADB enabled

### Launch Steps

1. **Start Windows app** (listens on port 5001):
   ```
   Data-Testing\Hand-Sight\out\Hand-Sight.exe
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

4. **Monitor Android Debug Output** (highly recommended):
   ```bash
   adb logcat -s HandSightCamera:I
   ```
   This shows real-time stats, connection status, frame counts, and throughput metrics.

5. **Verify Connection**:
   - Windows app should show: "Client connected. Receiving JPEG frames..."
   - Camera feed should appear in the Windows window
   - Phone camera should be streaming at 1280x960 JPEG @ ~30fps
   - Android logcat should show connection messages and streaming stats every 30 frames

### Status Messages

| Message | Meaning | Action |
|---------|---------|--------|
| "Waiting for adb reverse client..." | Ready but no phone connected | Run `adb reverse tcp:5001 tcp:5001` |
| "Client connected. Receiving JPEG frames..." | ✅ Streaming active | Video should display |
| "Connection lost. Waiting for reconnect..." | Network dropped | Restart Android app |
| "JPEG decode failed" | Frame corruption | Check network quality |

## Debug Features & Logging

The Android app includes comprehensive debug logging to monitor application behavior during testing. All output is sent to `logcat` and categorized by module:

### Android Debug Output Categories

| Category | Shows | Example |
|----------|-------|---------|
| `[INIT]` | App initialization and startup | "App version: 2.0 \| Protocol: HSF2v2" |
| `[APP]` | Lifecycle events (focus, pause, resume) | "Gained focus", "Camera permission granted" |
| `[CAMERA]` | Camera selection and configuration | "✓ Camera started: 1280x960 @ quality 92" |
| `[NETWORK]` | Network connection status | "✓ Connected to Windows receiver" |
| `[SEND]` | Frame transmission stats | "Frames: 30 \| Size: 27KB \| Total: 45.2 MB" |
| `[DEBUG-SEND]` | Per-frame throughput metrics | "Connected: YES \| Bytes: 542.3 KB/s \| FPS: 28.1" |
| `[MAIN]` | Main loop statistics (every 5s) | "Captured: 450 frames \| Sent: 420 frames" |
| `[SHUTDOWN]` | Graceful shutdown info | "Final stats - Captured: 1200 \| Sent: 1150" |

### How to Monitor

Watch all HandSightCamera debug output in real-time:
```bash
adb logcat -s HandSightCamera:I
```

Filter by specific category (e.g., network events):
```bash
adb logcat -s HandSightCamera:I | grep "\[NETWORK\]"
```

Save logs to file for analysis:
```bash
adb logcat -s HandSightCamera:I > handsight-debug.log
```

### Key Metrics Displayed

- **Connection Status**: Indicates if currently connected to Windows receiver
- **Throughput**: Real-time bytes per second (KB/s and MB/s)
- **Frame Rate**: Frames per second being transmitted
- **Frame Count**: Total frames captured vs. sent (helps detect drops)
- **Payload Size**: Bytes per JPEG frame
- **Camera Info**: Resolution, quality setting, zoom status

### Example Debug Output

```
[INIT] App version: 2.0 | Protocol: HSF2v2 | Target: Windows 127.0.0.1:5001
[CAMERA] ✓ Camera started successfully!
[CAMERA] Output: 1280x960 JPEG @ quality 92
[NETWORK] ✓ Connected to Windows receiver at 127.0.0.1:5001
[SEND] ✓ First frame sent: 45823 bytes | Total: 45.23 KB
[SEND] Frames: 30 | Size: 42100 bytes | Total: 891.5 MB | Captured: 450
[DEBUG-SEND] Connected: YES | Frames: 450 | Bytes: 542.30 KB/s (0.53 MB/s) | FPS: 28.1 | Status: CONNECTED
```

## Key Bug Fixes Applied

### Image Scaling Issue (Fixed 2026-05-22)
- **Problem**: Video appeared stretched/distorted
- **Cause**: ComputeFitRect() used `std::max()` instead of `std::min()` for scale calculation, causing oversized rendering
- **Fix**: Changed to proper aspect-ratio-preserving scaling with `std::min()` and removed forced minimum size constraints
- **Lines**: Data-Testing/Hand-Sight/Hand-Sight.cpp:130-133

### Android Debug UI (Added 2026-05-22)
- **Enhancement**: Comprehensive logcat logging with categorized output
- **Features**: Real-time metrics (throughput, FPS, frame counts), connection status, detailed error messages
- **Lines**: Data-Testing/android/HandSightCamera/jni/HandSightCamera.cpp (multiple locations)

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
