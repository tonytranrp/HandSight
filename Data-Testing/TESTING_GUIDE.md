# HandSight Data-Testing Guide

## Overview

The Data-Testing folder contains the complete camera streaming application for testing and development with comprehensive debug capabilities.

## Quick Start

### 1. Build & Launch Windows App
```bash
cd Data-Testing/Hand-Sight
mkdir out && cd out
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
# Launch the executable
.\Hand-Sight.exe
```

The Windows window will show: `"Waiting for adb reverse client on 127.0.0.1:5001..."`

### 2. Setup ADB Reverse Tunnel
```bash
adb reverse tcp:5001 tcp:5001
```

### 3. Build & Install Android App
```bash
cd Data-Testing/android/HandSightCamera
.\build.ps1 -Install
```

Or without auto-install:
```bash
.\build.ps1
adb install -r out/HandSightCamera-signed.apk
```

### 4. Launch Android App
```bash
adb shell am start -n "com.handsight.camera/android.app.NativeActivity"
```

### 5. Monitor Debug Output
```bash
adb logcat -s HandSightCamera:I
```

## Debug Output Examples

The Android app outputs comprehensive debug information to logcat in multiple categories:

### Real-Time Metrics (Every 30 Frames)

```
╔════════════════════════════════════════╗
║  📱 HANDSIGHT DEBUG HUD               ║
║ FPS: 29.9 | Sent: 1050  │
║ Captured: 1050 | Conn: YES  │
║ Throughput: 7.85 MB/s  │
║ Total: 245.5 MB  │
║ Status: CONNECTED  │
║ Resolution: 1280x960 | JPEG Q92  │
╚════════════════════════════════════════╝

[SEND] Frames: 1050 | Size: 251640 bytes | Total: 239.66 MB | Captured: 1050
[DEBUG-SEND] Connected: YES | Frames: 1050 | Bytes: 542.30 KB/s (0.53 MB/s) | FPS: 30.0 | Status: CONNECTED
```

### Periodic Stats (Every 5 Seconds)

```
[MAIN] === DEBUG STATS ===
[MAIN] Camera: 1028 frames captured | Network: 1028 frames sent | Connected: YES
[MAIN] Throughput: 7.85 MB/s | Frame rate: 30.0 FPS
```

### Connection Events

```
[NETWORK] ✓ Connected to Windows receiver at 127.0.0.1:5001
[NETWORK] ✗ Connection dropped. Reconnecting...
[NETWORK] ⏳ Attempt 1: Waiting for adb reverse on 127.0.0.1:5001...
```

### Camera Events

```
[CAMERA] ✓ Camera started successfully!
[CAMERA] Output: 1280x960 JPEG @ quality 92
[CAMERA] Ready to capture and stream frames
```

## Key Metrics Explained

| Metric | Description | Example |
|--------|-----------|---------|
| **FPS** | Frames per second being streamed | 29.9 FPS |
| **Sent** | Total frames transmitted | 1050 frames |
| **Captured** | Frames captured from camera | 1050 frames |
| **Conn** | Connection status | YES/NO |
| **Throughput** | Data transmission rate | 7.85 MB/s or 542 KB/s |
| **Total** | Total megabytes transmitted | 245.5 MB |
| **Status** | Network connection state | CONNECTED / DISCONNECTED / CONNECTING |

## Logcat Filtering

Watch only HUD output:
```bash
adb logcat -s HandSightCamera:I | grep "╔\|║\|╚\|HUD"
```

Watch only network events:
```bash
adb logcat -s HandSightCamera:I | grep "\[NETWORK\]"
```

Watch only throughput/FPS metrics:
```bash
adb logcat -s HandSightCamera:I | grep "\[DEBUG-SEND\]\|\[MAIN\]"
```

Save to file for offline analysis:
```bash
adb logcat -s HandSightCamera:I > handsight-session-$(date +%Y%m%d-%H%M%S).log
```

## Troubleshooting

### Windows app shows blank screen
- Verify ADB reverse is active: `adb reverse --list`
- Check Windows firewall allows port 5001
- Restart both apps

### Android app won't connect
- Verify ADB is connected: `adb devices`
- Check logcat for permission errors: `adb logcat -s HandSightCamera`
- Ensure camera permission is granted on phone

### No frames being sent
- Check camera permission in phone settings
- Verify phone has sufficient permissions: Settings > Apps > HandSight > Permissions
- Look for `[CAMERA]` messages in logcat

### Low throughput or high FPS variability
- Check network conditions (WiFi signal strength)
- Monitor USB bandwidth if using USB debugging
- Check phone CPU/battery status

## Performance Targets

- **Frame Rate**: 29-30 FPS (stable)
- **Frame Size**: 25-30 KB per frame (JPEG @ quality 92, varies with content)
- **Throughput**: 7-8 MB/s @ 30 FPS
- **Latency**: ~50-100ms from capture to display
- **Frame Drop Rate**: <1% under normal conditions

## Advanced Debugging

### Enable Verbose Logcat
```bash
adb logcat -s HandSightCamera:V
```

### Clear Old Logs
```bash
adb logcat -c
```

### Get Final Statistics on Exit
```bash
adb logcat -s HandSightCamera -d | tail -20
```

### Monitor in Real-Time with Grep
```bash
adb logcat -s HandSightCamera:I | tee handsight.log
```

## Architecture

- **Native C++**: Camera capture, JPEG encoding, network transmission
- **HSF2 Protocol**: Custom binary protocol for frame streaming
- **Windows Receiver**: GDI-based rendering with aspect-ratio-aware scaling
- **Multi-threaded**: Separate threads for camera, network, and UI

## Next Steps

The Android app currently displays debug info via logcat. Future enhancements could include:
- On-screen HUD overlay (see `DebugOverlay.kt` for UI layer skeleton)
- Graphical statistics display
- Network performance graphs
- Recording/playback functionality
