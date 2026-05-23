#include <android/log.h>
#include <android_native_app_glue.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../../shared/HandSightStreamProtocol.h"

#define LOG_TAG "HandSightCamera"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace
{
    constexpr char kServerIp[] = "127.0.0.1";
    constexpr std::uint16_t kServerPort = 5001;
    constexpr std::uint8_t kJpegQuality = 70;  // Reduced from 92 for faster decoding
    constexpr float kRequestedZoomRatio = 0.6f;
    constexpr int32_t kMaxImages = 3;

    struct FramePacket
    {
        std::uint64_t timestampUs = 0;
        std::vector<std::uint8_t> payload;
    };

    struct CameraContext
    {
        android_app* app = nullptr;
        ACameraManager* manager = nullptr;
        ACameraDevice* device = nullptr;
        ACameraCaptureSession* session = nullptr;
        ACaptureRequest* request = nullptr;
        ACaptureSessionOutputContainer* outputs = nullptr;
        ACaptureSessionOutput* output = nullptr;
        ACameraOutputTarget* target = nullptr;
        AImageReader* reader = nullptr;
        ANativeWindow* readerWindow = nullptr;
        std::string cameraId;
        std::uint32_t streamWidth = 0;
        std::uint32_t streamHeight = 0;
        std::uint32_t rotationDegrees = 0;
        float appliedZoomRatio = 1.0f;
        bool zoomRatioApplied = false;
        std::atomic<bool> running{ false };
        std::atomic<bool> firstFrameLogged{ false };
        std::atomic<std::uint64_t> capturedFrames{ 0 };
        std::atomic<std::uint64_t> sentFrames{ 0 };
        std::mutex queueMutex;
        std::condition_variable queueCv;
        std::deque<FramePacket> frameQueue;
    };

    struct DebugHUD
    {
        std::string line1 = "";
        std::string line2 = "";
        std::string line3 = "";
        std::string line4 = "";
        std::string line5 = "";
        std::string line6 = "";
        std::mutex hudMutex;
    };

    struct NetworkStats
    {
        std::uint64_t totalBytesSent = 0;
        std::uint64_t framesAtLastSample = 0;
        std::chrono::steady_clock::time_point lastSampleTime;
        double currentBytesSec = 0.0;
        double currentFramesSec = 0.0;
        std::atomic<bool> isConnected{ false };
        std::atomic<int> packetLossPercent{ 0 };
        std::string connectionStatus = "Disconnected";
        std::mutex statsMutex;
        DebugHUD debugHUD;
    };

    struct AppState
    {
        android_app* app = nullptr;
        std::atomic<bool> running{ true };
        std::atomic<bool> cameraStarted{ false };
        std::thread networkThread;
        CameraContext camera;
        NetworkStats netStats;
    };

    AppState* g_app = nullptr;

    static void StopCamera(CameraContext& camera);

    static void SetStatus(const char* text)
    {
        LOGI("[STATUS] %s", text);
    }

    static void UpdateNetworkStats(NetworkStats& stats, std::uint64_t sentBytes, std::uint64_t sentFrames, bool connected)
    {
        std::lock_guard<std::mutex> lock(stats.statsMutex);
        stats.totalBytesSent = sentBytes;
        stats.isConnected.store(connected);

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - stats.lastSampleTime);

        if (elapsed.count() >= 1000)
        {
            const std::uint64_t framesSinceSample = sentFrames - stats.framesAtLastSample;
            const std::uint64_t bytesSinceSample = sentBytes >= stats.totalBytesSent ? sentBytes - stats.totalBytesSent : sentBytes;

            stats.currentFramesSec = static_cast<double>(framesSinceSample) * 1000.0 / static_cast<double>(elapsed.count());
            stats.currentBytesSec = static_cast<double>(bytesSinceSample) * 1000.0 / static_cast<double>(elapsed.count());

            stats.framesAtLastSample = sentFrames;
            stats.lastSampleTime = now;
        }
    }

    static void UpdateHUD(NetworkStats& netStats, const CameraContext& camera)
    {
        std::lock_guard<std::mutex> lock(netStats.debugHUD.hudMutex);

        char buffer[256];

        snprintf(buffer, sizeof(buffer), "FPS: %.1f | Sent: %llu",
                 netStats.currentFramesSec,
                 static_cast<unsigned long long>(camera.sentFrames.load()));
        netStats.debugHUD.line1 = buffer;

        snprintf(buffer, sizeof(buffer), "Captured: %llu | Conn: %s",
                 static_cast<unsigned long long>(camera.capturedFrames.load()),
                 netStats.isConnected.load() ? "YES" : "NO");
        netStats.debugHUD.line2 = buffer;

        snprintf(buffer, sizeof(buffer), "Throughput: %.2f MB/s",
                 netStats.currentBytesSec / (1024.0 * 1024.0));
        netStats.debugHUD.line3 = buffer;

        snprintf(buffer, sizeof(buffer), "Total: %.2f MB",
                 netStats.totalBytesSent / (1024.0 * 1024.0));
        netStats.debugHUD.line4 = buffer;

        snprintf(buffer, sizeof(buffer), "Status: %s", netStats.connectionStatus.c_str());
        netStats.debugHUD.line5 = buffer;

        snprintf(buffer, sizeof(buffer), "Resolution: 1280x960 | JPEG Q92");
        netStats.debugHUD.line6 = buffer;
    }

    static void LogDebugStats(AppState* app, const char* context)
    {
        if (app == nullptr)
            return;

        std::lock_guard<std::mutex> lock(app->netStats.statsMutex);
        LOGI("[DEBUG-%s] Connected: %s | Frames: %llu | Bytes: %.2f KB/s (%.2f MB/s) | FPS: %.1f | Status: %s",
             context,
             app->netStats.isConnected.load() ? "YES" : "NO",
             static_cast<unsigned long long>(app->camera.sentFrames.load()),
             app->netStats.currentBytesSec / 1024.0,
             app->netStats.currentBytesSec / (1024.0 * 1024.0),
             app->netStats.currentFramesSec,
             app->netStats.connectionStatus.c_str());
    }

    static bool SendAll(int socketFd, const void* buffer, size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(buffer);
        size_t sent = 0;
        while (sent < size)
        {
            const ssize_t result = send(socketFd, bytes + sent, size - sent, 0);
            if (result > 0)
            {
                sent += static_cast<size_t>(result);
                continue;
            }

            if (result == 0)
            {
                return false;
            }

            if (errno == EINTR)
            {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return false;
            }

            return false;
        }
        return true;
    }

    static bool AcquireJniEnv(android_app* app, JNIEnv** env, bool* attached)
    {
        if (env == nullptr || attached == nullptr || app == nullptr || app->activity == nullptr || app->activity->vm == nullptr)
        {
            return false;
        }

        *env = nullptr;
        *attached = false;
        if (app->activity->vm->GetEnv(reinterpret_cast<void**>(env), JNI_VERSION_1_6) == JNI_OK)
        {
            return true;
        }

        if (app->activity->vm->AttachCurrentThread(env, nullptr) != JNI_OK)
        {
            return false;
        }

        *attached = true;
        return true;
    }

    static int32_t GetDisplayRotationDegrees(android_app* app)
    {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (!AcquireJniEnv(app, &env, &attached))
        {
            return 0;
        }

        int32_t rotationDegrees = 0;
        jclass activityClass = nullptr;
        jclass windowManagerClass = nullptr;
        jclass displayClass = nullptr;
        jobject windowManager = nullptr;
        jobject display = nullptr;

        do
        {
            activityClass = env->GetObjectClass(app->activity->clazz);
            if (activityClass == nullptr)
            {
                break;
            }

            const jmethodID getWindowManager = env->GetMethodID(activityClass, "getWindowManager", "()Landroid/view/WindowManager;");
            if (getWindowManager == nullptr)
            {
                break;
            }

            windowManager = env->CallObjectMethod(app->activity->clazz, getWindowManager);
            if (windowManager == nullptr)
            {
                break;
            }

            windowManagerClass = env->GetObjectClass(windowManager);
            if (windowManagerClass == nullptr)
            {
                break;
            }

            const jmethodID getDefaultDisplay = env->GetMethodID(windowManagerClass, "getDefaultDisplay", "()Landroid/view/Display;");
            if (getDefaultDisplay == nullptr)
            {
                break;
            }

            display = env->CallObjectMethod(windowManager, getDefaultDisplay);
            if (display == nullptr)
            {
                break;
            }

            displayClass = env->GetObjectClass(display);
            if (displayClass == nullptr)
            {
                break;
            }

            const jmethodID getRotation = env->GetMethodID(displayClass, "getRotation", "()I");
            if (getRotation == nullptr)
            {
                break;
            }

            const jint rotation = env->CallIntMethod(display, getRotation);
            switch (rotation)
            {
            case 0:
                rotationDegrees = 0;
                break;
            case 1:
                rotationDegrees = 90;
                break;
            case 2:
                rotationDegrees = 180;
                break;
            case 3:
                rotationDegrees = 270;
                break;
            default:
                rotationDegrees = 0;
                break;
            }
        } while (false);

        if (displayClass)
        {
            env->DeleteLocalRef(displayClass);
        }
        if (display)
        {
            env->DeleteLocalRef(display);
        }
        if (windowManagerClass)
        {
            env->DeleteLocalRef(windowManagerClass);
        }
        if (windowManager)
        {
            env->DeleteLocalRef(windowManager);
        }
        if (activityClass)
        {
            env->DeleteLocalRef(activityClass);
        }

        if (attached)
        {
            app->activity->vm->DetachCurrentThread();
        }

        return rotationDegrees;
    }

    static bool HasCameraPermission(android_app* app)
    {
        if (app == nullptr || app->activity == nullptr || app->activity->vm == nullptr)
        {
            return false;
        }

        JNIEnv* env = nullptr;
        bool attached = false;
        if (!AcquireJniEnv(app, &env, &attached))
        {
            return false;
        }

        bool granted = false;
        jclass activityClass = nullptr;
        jstring permissionName = nullptr;

        do
        {
            activityClass = env->GetObjectClass(app->activity->clazz);
            if (activityClass == nullptr)
            {
                break;
            }

            const jmethodID checkSelfPermission = env->GetMethodID(activityClass, "checkSelfPermission", "(Ljava/lang/String;)I");
            if (checkSelfPermission == nullptr)
            {
                break;
            }

            permissionName = env->NewStringUTF("android.permission.CAMERA");
            if (permissionName == nullptr)
            {
                break;
            }

            const jint result = env->CallIntMethod(app->activity->clazz, checkSelfPermission, permissionName);
            granted = (result == 0);
        } while (false);

        if (permissionName)
        {
            env->DeleteLocalRef(permissionName);
        }
        if (activityClass)
        {
            env->DeleteLocalRef(activityClass);
        }
        if (attached)
        {
            app->activity->vm->DetachCurrentThread();
        }

        return granted;
    }

    static bool GetMetadataEntry(ACameraMetadata* metadata, uint32_t tag, ACameraMetadata_const_entry* entry)
    {
        return metadata != nullptr && entry != nullptr && ACameraMetadata_getConstEntry(metadata, tag, entry) == ACAMERA_OK;
    }

    static int32_t GetUseCasePriority(int32_t supportedUsecases)
    {
        if (supportedUsecases & (1 << ACAMERA_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS_LOW_LATENCY_SNAPSHOT))
        {
            return 3;
        }
        if (supportedUsecases & (1 << ACAMERA_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS_SNAPSHOT))
        {
            return 2;
        }
        if (supportedUsecases & (1 << ACAMERA_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS_VIDEO_SNAPSHOT))
        {
            return 1;
        }
        if (supportedUsecases & (1 << ACAMERA_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS_PREVIEW))
        {
            return 0;
        }
        return -1;
    }

    struct StreamChoice
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        int32_t priority = -1;
        double aspectDiff = std::numeric_limits<double>::max();
        std::int64_t area = -1;
        bool valid = false;
    };

    struct CameraCandidate
    {
        std::string cameraId;
        float focalLength = std::numeric_limits<float>::max();
        int32_t sensorOrientation = 90;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        double activeAspect = 0.0;
        float zoomMin = 1.0f;
        float zoomMax = 1.0f;
        bool isBackFacing = false;
        bool zoomSupported = false;
        bool valid = false;
    };

    static bool ChooseJpegStreamSizeFromRecommended(ACameraMetadata* metadata, double activeAspect, StreamChoice& outChoice)
    {
        ACameraMetadata_const_entry entry{};
        if (!GetMetadataEntry(metadata, ACAMERA_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS, &entry) || entry.count < 5)
        {
            return false;
        }

        const int32_t* values = entry.data.i32;
        bool found = false;
        for (size_t index = 0; index + 4 < entry.count; index += 5)
        {
            const int32_t width = values[index + 0];
            const int32_t height = values[index + 1];
            const int32_t format = values[index + 2];
            const int32_t isInput = values[index + 3];
            const int32_t supportedUsecases = values[index + 4];

            if (isInput != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT)
            {
                continue;
            }
            if (format != AIMAGE_FORMAT_JPEG)
            {
                continue;
            }

            const int32_t priority = GetUseCasePriority(supportedUsecases);
            if (priority < 0)
            {
                continue;
            }

            const std::int64_t area = static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height);
            const double aspect = height > 0 ? static_cast<double>(width) / static_cast<double>(height) : 0.0;
            const double aspectDiff = activeAspect > 0.0 ? std::abs(aspect - activeAspect) : 0.0;

            if (!found ||
                priority > outChoice.priority ||
                (priority == outChoice.priority &&
                 (aspectDiff < outChoice.aspectDiff - 1e-6 ||
                  (std::abs(aspectDiff - outChoice.aspectDiff) <= 1e-6 && area > outChoice.area))))
            {
                outChoice.width = static_cast<std::uint32_t>(width);
                outChoice.height = static_cast<std::uint32_t>(height);
                outChoice.priority = priority;
                outChoice.aspectDiff = aspectDiff;
                outChoice.area = area;
                outChoice.valid = true;
                found = true;
            }
        }

        return found;
    }

    static bool ChooseJpegStreamSizeFromAvailable(ACameraMetadata* metadata, double activeAspect, StreamChoice& outChoice)
    {
        ACameraMetadata_const_entry entry{};
        if (!GetMetadataEntry(metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry) || entry.count < 4)
        {
            return false;
        }

        const int32_t* values = entry.data.i32;
        bool found = false;
        for (size_t index = 0; index + 3 < entry.count; index += 4)
        {
            const int32_t width = values[index + 0];
            const int32_t height = values[index + 1];
            const int32_t format = values[index + 2];
            const int32_t isInput = values[index + 3];

            if (isInput != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT)
            {
                continue;
            }
            if (format != AIMAGE_FORMAT_JPEG)
            {
                continue;
            }

            const std::int64_t area = static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height);
            const double aspect = height > 0 ? static_cast<double>(width) / static_cast<double>(height) : 0.0;
            const double aspectDiff = activeAspect > 0.0 ? std::abs(aspect - activeAspect) : 0.0;

            if (!found ||
                aspectDiff < outChoice.aspectDiff - 1e-6 ||
                (std::abs(aspectDiff - outChoice.aspectDiff) <= 1e-6 && area > outChoice.area))
            {
                outChoice.width = static_cast<std::uint32_t>(width);
                outChoice.height = static_cast<std::uint32_t>(height);
                outChoice.priority = 0;
                outChoice.aspectDiff = aspectDiff;
                outChoice.area = area;
                outChoice.valid = true;
                found = true;
            }
        }

        return found;
    }

    static void ChooseFallbackJpegStreamSize(double activeAspect, StreamChoice& outChoice)
    {
        static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 6> kFallbackSizes = {{
            {1920u, 1080u},
            {1600u, 900u},
            {1280u, 720u},
            {1280u, 960u},
            {960u, 540u},
            {640u, 480u},
        }};

        bool found = false;
        for (const auto& size : kFallbackSizes)
        {
            const double aspect = size.second > 0 ? static_cast<double>(size.first) / static_cast<double>(size.second) : 0.0;
            const double aspectDiff = activeAspect > 0.0 ? std::abs(aspect - activeAspect) : 0.0;
            const std::int64_t area = static_cast<std::int64_t>(size.first) * static_cast<std::int64_t>(size.second);

            if (!found ||
                aspectDiff < outChoice.aspectDiff - 1e-6 ||
                (std::abs(aspectDiff - outChoice.aspectDiff) <= 1e-6 && area > outChoice.area))
            {
                outChoice.width = size.first;
                outChoice.height = size.second;
                outChoice.priority = 0;
                outChoice.aspectDiff = aspectDiff;
                outChoice.area = area;
                outChoice.valid = true;
                found = true;
            }
        }
    }

    static bool SelectCamera(CameraContext& camera)
    {
        camera.manager = ACameraManager_create();
        if (camera.manager == nullptr)
        {
            LOGE("Failed to create camera manager");
            return false;
        }

        ACameraIdList* cameraIds = nullptr;
        if (ACameraManager_getCameraIdList(camera.manager, &cameraIds) != ACAMERA_OK || cameraIds == nullptr)
        {
            LOGE("Failed to enumerate cameras");
            return false;
        }

        std::string firstCameraId;
        if (cameraIds->numCameras > 0 && cameraIds->cameraIds[0] != nullptr)
        {
            firstCameraId = cameraIds->cameraIds[0];
        }

        CameraCandidate bestBackCamera{};
        CameraCandidate bestAnyCamera{};
        bool haveBackCamera = false;
        bool haveAnyCamera = false;

        for (int32_t index = 0; index < cameraIds->numCameras; ++index)
        {
            const char* cameraId = cameraIds->cameraIds[index];
            LOGI("Inspecting camera %s", cameraId != nullptr ? cameraId : "<null>");

            ACameraMetadata* metadata = nullptr;
            if (ACameraManager_getCameraCharacteristics(camera.manager, cameraId, &metadata) != ACAMERA_OK || metadata == nullptr)
            {
                LOGI("  Failed to read characteristics");
                continue;
            }

            CameraCandidate candidate{};
            candidate.cameraId = cameraId != nullptr ? cameraId : "";

            ACameraMetadata_const_entry lensFacing{};
            if (GetMetadataEntry(metadata, ACAMERA_LENS_FACING, &lensFacing) && lensFacing.count > 0)
            {
                candidate.isBackFacing = lensFacing.data.u8[0] == ACAMERA_LENS_FACING_BACK;
            }

            ACameraMetadata_const_entry focalLengths{};
            if (GetMetadataEntry(metadata, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &focalLengths) && focalLengths.count > 0)
            {
                for (size_t focalIndex = 0; focalIndex < focalLengths.count; ++focalIndex)
                {
                    candidate.focalLength = std::min(candidate.focalLength, focalLengths.data.f[focalIndex]);
                }
            }

            ACameraMetadata_const_entry sensorOrientation{};
            if (GetMetadataEntry(metadata, ACAMERA_SENSOR_ORIENTATION, &sensorOrientation) && sensorOrientation.count > 0)
            {
                candidate.sensorOrientation = sensorOrientation.data.i32[0];
            }

            ACameraMetadata_const_entry activeArray{};
            if (GetMetadataEntry(metadata, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &activeArray) && activeArray.count >= 4)
            {
                const int32_t left = activeArray.data.i32[0];
                const int32_t top = activeArray.data.i32[1];
                const int32_t right = activeArray.data.i32[2];
                const int32_t bottom = activeArray.data.i32[3];
                const int32_t activeWidth = std::max(1, right - left);
                const int32_t activeHeight = std::max(1, bottom - top);
                candidate.activeAspect = static_cast<double>(activeWidth) / static_cast<double>(activeHeight);
            }

            StreamChoice streamChoice{};
            if (!ChooseJpegStreamSizeFromRecommended(metadata, candidate.activeAspect, streamChoice))
            {
                ChooseJpegStreamSizeFromAvailable(metadata, candidate.activeAspect, streamChoice);
            }

            if (!streamChoice.valid)
            {
                LOGI("  No JPEG stream size metadata found; using fallback stream sizes.");
                ChooseFallbackJpegStreamSize(candidate.activeAspect, streamChoice);
            }

            ACameraMetadata_const_entry zoomRange{};
            if (GetMetadataEntry(metadata, ACAMERA_CONTROL_ZOOM_RATIO_RANGE, &zoomRange) && zoomRange.count >= 2)
            {
                candidate.zoomMin = zoomRange.data.f[0];
                candidate.zoomMax = zoomRange.data.f[1];
                if (candidate.zoomMin <= kRequestedZoomRatio && kRequestedZoomRatio <= candidate.zoomMax)
                {
                    candidate.zoomSupported = true;
                }
            }

            candidate.width = streamChoice.width;
            candidate.height = streamChoice.height;
            candidate.valid = candidate.width > 0 && candidate.height > 0;

            ACameraMetadata_free(metadata);

            if (!candidate.valid)
            {
                LOGI("  Skipping camera %s because no usable output size was found", candidate.cameraId.c_str());
                continue;
            }

            LOGI("  facing=%s focal=%.2fmm sensorRotation=%d size=%ux%u zoomRange=%.2f..%.2f",
                 candidate.isBackFacing ? "back" : "other",
                 candidate.focalLength,
                 candidate.sensorOrientation,
                 candidate.width,
                 candidate.height,
                 candidate.zoomMin,
                 candidate.zoomMax);

            if (!haveAnyCamera || candidate.focalLength < bestAnyCamera.focalLength)
            {
                bestAnyCamera = candidate;
                haveAnyCamera = true;
            }

            if (candidate.isBackFacing && (!haveBackCamera || candidate.focalLength < bestBackCamera.focalLength))
            {
                bestBackCamera = candidate;
                haveBackCamera = true;
            }
        }

        ACameraManager_deleteCameraIdList(cameraIds);

        CameraCandidate selected = haveBackCamera ? bestBackCamera : bestAnyCamera;
        if (!selected.valid)
        {
            LOGE("Could not find a usable camera");
            return false;
        }

        if (!haveBackCamera)
        {
            LOGI("No back-facing camera metadata match was found; using %s as a fallback.",
                 selected.cameraId.c_str());
        }

        if (selected.cameraId.empty() || selected.width == 0 || selected.height == 0)
        {
            if (!firstCameraId.empty())
            {
                selected.cameraId = firstCameraId;
            }
            if (selected.width == 0 || selected.height == 0)
            {
                selected.width = 1920;
                selected.height = 1080;
            }
        }

        camera.cameraId = selected.cameraId;
        camera.streamWidth = selected.width;
        camera.streamHeight = selected.height;
        camera.appliedZoomRatio = selected.zoomSupported ? kRequestedZoomRatio : 1.0f;
        camera.zoomRatioApplied = selected.zoomSupported;

        const int32_t displayRotation = GetDisplayRotationDegrees(camera.app);
        const int32_t sensorRotation = ((selected.sensorOrientation % 360) + 360) % 360;
        const int32_t deviceRotation = ((displayRotation % 360) + 360) % 360;
        camera.rotationDegrees = static_cast<std::uint32_t>((sensorRotation - deviceRotation + 360) % 360);

        LOGI("Selected camera %s at %ux%u, focal=%.2fmm, sensorRotation=%d, displayRotation=%d, streamRotation=%u, zoom=%s (range %.2f..%.2f)",
             camera.cameraId.c_str(),
             camera.streamWidth,
             camera.streamHeight,
             selected.focalLength,
             selected.sensorOrientation,
             displayRotation,
             camera.rotationDegrees,
             camera.zoomRatioApplied ? "0.6x" : "wide-lens fallback",
             selected.zoomMin,
             selected.zoomMax);
        return true;
    }

    static bool CopyJpegImage(AImage* image, FramePacket& packet)
    {
        int32_t planeCount = 0;
        if (AImage_getNumberOfPlanes(image, &planeCount) != AMEDIA_OK || planeCount < 1)
        {
            return false;
        }

        uint8_t* planeData = nullptr;
        int planeDataLength = 0;
        if (AImage_getPlaneData(image, 0, &planeData, &planeDataLength) != AMEDIA_OK || planeData == nullptr || planeDataLength <= 0)
        {
            return false;
        }

        packet.payload.assign(planeData, planeData + planeDataLength);

        int64_t timestampNs = 0;
        if (AImage_getTimestamp(image, &timestampNs) == AMEDIA_OK && timestampNs > 0)
        {
            packet.timestampUs = static_cast<std::uint64_t>(timestampNs / 1000);
        }
        else
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            packet.timestampUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        }

        return true;
    }

    static void OnImageAvailable(void* context, AImageReader* reader)
    {
        auto* camera = static_cast<CameraContext*>(context);
        if (camera == nullptr || !camera->running.load())
        {
            return;
        }

        AImage* image = nullptr;
        if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || image == nullptr)
        {
            return;
        }

        FramePacket packet;
        if (CopyJpegImage(image, packet))
        {
            const std::uint64_t capturedCount = camera->capturedFrames.fetch_add(1) + 1;
            if (capturedCount % 30u == 0u)
            {
                LOGI("Captured %llu camera frames", static_cast<unsigned long long>(capturedCount));
            }

            if (!camera->firstFrameLogged.exchange(true))
            {
                LOGI("First camera frame received: %zu bytes", packet.payload.size());
            }

            std::lock_guard<std::mutex> lock(camera->queueMutex);
            camera->frameQueue.clear();
            camera->frameQueue.push_back(std::move(packet));
            camera->queueCv.notify_one();
        }

        AImage_delete(image);
    }

    static bool ConnectSocket(int& socketFd)
    {
        socketFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socketFd < 0)
        {
            return false;
        }

        const int tcpNoDelay = 1;
        setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &tcpNoDelay, sizeof(tcpNoDelay));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kServerPort);
        if (inet_pton(AF_INET, kServerIp, &addr.sin_addr) != 1)
        {
            close(socketFd);
            socketFd = -1;
            return false;
        }

        while (g_app != nullptr && g_app->running.load())
        {
            if (connect(socketFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            {
                timeval timeout{};
                timeout.tv_sec = 1;
                timeout.tv_usec = 0;
                setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                return true;
            }

            if (errno != ECONNREFUSED && errno != ENETUNREACH && errno != ETIMEDOUT && errno != EINPROGRESS)
            {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        close(socketFd);
        socketFd = -1;
        return false;
    }

    static bool SendFrameStream(int socketFd, CameraContext& camera)
    {
        const handsight::StreamHeader header{
            handsight::kStreamMagic,
            handsight::kStreamVersion,
            camera.streamWidth,
            camera.streamHeight,
            handsight::kPixelFormatJpeg,
            camera.rotationDegrees,
        };

        if (!SendAll(socketFd, &header, sizeof(header)))
        {
            LOGE("[SEND] Failed to send stream header");
            return false;
        }

        LOGI("[STREAM] Header sent: %ux%u, rotation=%u°", camera.streamWidth, camera.streamHeight, camera.rotationDegrees);
        if (g_app != nullptr)
        {
            g_app->netStats.connectionStatus = "CONNECTED";
            g_app->netStats.isConnected.store(true);
        }

        auto lastDebugTime = std::chrono::steady_clock::now();
        std::uint64_t totalBytesSent = sizeof(header);

        while (g_app != nullptr && g_app->running.load())
        {
            FramePacket packet;
            {
                std::unique_lock<std::mutex> lock(camera.queueMutex);
                camera.queueCv.wait(lock, [&]()
                {
                    return g_app == nullptr || !g_app->running.load() || !camera.frameQueue.empty();
                });

                if (g_app == nullptr || !g_app->running.load())
                {
                    return false;
                }

                packet = std::move(camera.frameQueue.back());
                camera.frameQueue.clear();
            }

            if (packet.payload.empty())
            {
                continue;
            }

            const handsight::FrameHeader frameHeader{
                static_cast<std::uint32_t>(packet.payload.size()),
                packet.timestampUs,
            };

            if (!SendAll(socketFd, &frameHeader, sizeof(frameHeader)))
            {
                LOGE("[SEND] Failed to send frame header (frame #%llu, %u bytes)",
                     static_cast<unsigned long long>(camera.sentFrames.load()), frameHeader.payloadSize);
                return false;
            }
            if (!SendAll(socketFd, packet.payload.data(), packet.payload.size()))
            {
                LOGE("[SEND] Failed to send frame payload (frame #%llu, %u bytes)",
                     static_cast<unsigned long long>(camera.sentFrames.load()), frameHeader.payloadSize);
                return false;
            }

            totalBytesSent += sizeof(frameHeader) + frameHeader.payloadSize;

            static std::atomic<bool> firstFrameSentLogged{ false };
            if (!firstFrameSentLogged.exchange(true))
            {
                LOGI("[SEND] ✓ First frame sent: %u bytes | Total: %.2f KB", frameHeader.payloadSize, totalBytesSent / 1024.0);
            }

            const std::uint64_t sentCount = camera.sentFrames.fetch_add(1) + 1;
            if (sentCount % 30u == 0u)
            {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastDebugTime);

                if (g_app != nullptr)
                {
                    UpdateNetworkStats(g_app->netStats, totalBytesSent, sentCount, true);
                    UpdateHUD(g_app->netStats, camera);
                    LogDebugStats(g_app, "SEND");

                    std::lock_guard<std::mutex> lock(g_app->netStats.debugHUD.hudMutex);
                    LOGI("╔════════════════════════════════════════╗");
                    LOGI("║  📱 HANDSIGHT DEBUG HUD               ║");
                    LOGI("║ %s  │", g_app->netStats.debugHUD.line1.c_str());
                    LOGI("║ %s  │", g_app->netStats.debugHUD.line2.c_str());
                    LOGI("║ %s  │", g_app->netStats.debugHUD.line3.c_str());
                    LOGI("║ %s  │", g_app->netStats.debugHUD.line4.c_str());
                    LOGI("║ %s  │", g_app->netStats.debugHUD.line5.c_str());
                    LOGI("║ %s  │", g_app->netStats.debugHUD.line6.c_str());
                    LOGI("╚════════════════════════════════════════╝");
                }

                LOGI("[SEND] Frames: %llu | Size: %u bytes | Total: %.2f MB | Captured: %llu",
                     static_cast<unsigned long long>(sentCount),
                     frameHeader.payloadSize,
                     totalBytesSent / (1024.0 * 1024.0),
                     static_cast<unsigned long long>(camera.capturedFrames.load()));
            }
        }

        return false;
    }

    static bool StartCamera(CameraContext& camera)
    {
        StopCamera(camera);

        camera.manager = ACameraManager_create();
        if (camera.manager == nullptr)
        {
            LOGE("Failed to create camera manager");
            return false;
        }

        camera.running.store(false);
        if (!SelectCamera(camera))
        {
            StopCamera(camera);
            return false;
        }

        if (AImageReader_new(static_cast<int32_t>(camera.streamWidth), static_cast<int32_t>(camera.streamHeight), AIMAGE_FORMAT_JPEG, kMaxImages, &camera.reader) != AMEDIA_OK)
        {
            LOGE("Failed to create JPEG image reader");
            StopCamera(camera);
            return false;
        }

        AImageReader_ImageListener listener{};
        listener.context = &camera;
        listener.onImageAvailable = OnImageAvailable;
        AImageReader_setImageListener(camera.reader, &listener);

        if (AImageReader_getWindow(camera.reader, &camera.readerWindow) != AMEDIA_OK)
        {
            LOGE("Failed to get reader window");
            StopCamera(camera);
            return false;
        }

        if (ACaptureSessionOutputContainer_create(&camera.outputs) != ACAMERA_OK)
        {
            LOGE("Failed to create output container");
            StopCamera(camera);
            return false;
        }

        if (ACaptureSessionOutput_create(camera.readerWindow, &camera.output) != ACAMERA_OK)
        {
            LOGE("Failed to create session output");
            StopCamera(camera);
            return false;
        }

        if (ACaptureSessionOutputContainer_add(camera.outputs, camera.output) != ACAMERA_OK)
        {
            LOGE("Failed to add output to container");
            StopCamera(camera);
            return false;
        }

        ACameraDevice_StateCallbacks deviceCallbacks{};
        if (ACameraManager_openCamera(camera.manager, camera.cameraId.c_str(), &deviceCallbacks, &camera.device) != ACAMERA_OK)
        {
            LOGE("Failed to open camera");
            StopCamera(camera);
            return false;
        }

        if (ACameraDevice_createCaptureRequest(camera.device, TEMPLATE_PREVIEW, &camera.request) != ACAMERA_OK)
        {
            LOGE("Failed to create capture request");
            StopCamera(camera);
            return false;
        }

        const uint8_t controlMode = ACAMERA_CONTROL_MODE_AUTO;
        const uint8_t captureIntent = ACAMERA_CONTROL_CAPTURE_INTENT_PREVIEW;
        const uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_ON;
        const uint8_t afMode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        const uint8_t awbMode = ACAMERA_CONTROL_AWB_MODE_AUTO;
        const uint8_t jpegQuality = kJpegQuality;
        const int32_t jpegOrientation = 0;
        const int32_t jpegThumbnailSize[] = { 0, 0 };

        ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_MODE, 1, &controlMode);
        ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_CAPTURE_INTENT, 1, &captureIntent);
        ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
        ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
        ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_AWB_MODE, 1, &awbMode);
        ACaptureRequest_setEntry_u8(camera.request, ACAMERA_JPEG_QUALITY, 1, &jpegQuality);
        ACaptureRequest_setEntry_i32(camera.request, ACAMERA_JPEG_ORIENTATION, 1, &jpegOrientation);
        ACaptureRequest_setEntry_i32(camera.request, ACAMERA_JPEG_THUMBNAIL_SIZE, 2, jpegThumbnailSize);

        if (camera.zoomRatioApplied)
        {
            const float zoomRatio = camera.appliedZoomRatio;
            ACaptureRequest_setEntry_float(camera.request, ACAMERA_CONTROL_ZOOM_RATIO, 1, &zoomRatio);
        }

        if (ACameraOutputTarget_create(camera.readerWindow, &camera.target) != ACAMERA_OK)
        {
            LOGE("Failed to create output target");
            StopCamera(camera);
            return false;
        }

        if (ACaptureRequest_addTarget(camera.request, camera.target) != ACAMERA_OK)
        {
            LOGE("Failed to add request target");
            StopCamera(camera);
            return false;
        }

        ACameraCaptureSession_stateCallbacks sessionCallbacks{};
        if (ACameraDevice_createCaptureSession(camera.device, camera.outputs, &sessionCallbacks, &camera.session) != ACAMERA_OK)
        {
            LOGE("Failed to create capture session");
            StopCamera(camera);
            return false;
        }

        ACaptureRequest* requests[] = { camera.request };
        if (ACameraCaptureSession_setRepeatingRequest(camera.session, nullptr, 1, requests, nullptr) != ACAMERA_OK)
        {
            LOGE("Failed to start repeating request");
            StopCamera(camera);
            return false;
        }

        camera.running.store(true);
        LOGI("[CAMERA] ✓ Camera started successfully!");
        LOGI("[CAMERA] Output: %ux%u JPEG @ quality %d", camera.streamWidth, camera.streamHeight, kJpegQuality);
        LOGI("[CAMERA] Zoom: %s (range %.2f-%.2f)", camera.zoomRatioApplied ? "ENABLED (0.6x)" : "DISABLED", 1.0f, 8.0f);
        LOGI("[CAMERA] Ready to capture and stream frames");
        return true;
    }

    static void StopCamera(CameraContext& camera)
    {
        camera.running.store(false);
        camera.queueCv.notify_all();

        if (camera.session)
        {
            ACameraCaptureSession_stopRepeating(camera.session);
            ACameraCaptureSession_close(camera.session);
            camera.session = nullptr;
        }
        if (camera.request)
        {
            ACaptureRequest_free(camera.request);
            camera.request = nullptr;
        }
        if (camera.target)
        {
            ACameraOutputTarget_free(camera.target);
            camera.target = nullptr;
        }
        if (camera.outputs)
        {
            ACaptureSessionOutputContainer_free(camera.outputs);
            camera.outputs = nullptr;
        }
        if (camera.output)
        {
            ACaptureSessionOutput_free(camera.output);
            camera.output = nullptr;
        }
        if (camera.reader)
        {
            AImageReader_delete(camera.reader);
            camera.reader = nullptr;
        }
        camera.readerWindow = nullptr;
        if (camera.device)
        {
            ACameraDevice_close(camera.device);
            camera.device = nullptr;
        }
        if (camera.manager)
        {
            ACameraManager_delete(camera.manager);
            camera.manager = nullptr;
        }
        camera.cameraId.clear();
        camera.streamWidth = 0;
        camera.streamHeight = 0;
        camera.rotationDegrees = 0;
        camera.appliedZoomRatio = 1.0f;
        camera.zoomRatioApplied = false;
        camera.firstFrameLogged.store(false);
        camera.capturedFrames.store(0);
        camera.sentFrames.store(0);

        std::lock_guard<std::mutex> lock(camera.queueMutex);
        camera.frameQueue.clear();
    }

    static void NetworkLoop(AppState* app)
    {
        int reconnectAttempts = 0;
        while (app != nullptr && app->running.load())
        {
            int socketFd = -1;
            if (!ConnectSocket(socketFd))
            {
                reconnectAttempts++;
                LOGI("[NETWORK] ⏳ Attempt %d: Waiting for adb reverse on 127.0.0.1:5001...", reconnectAttempts);
                SetStatus("Waiting for adb reverse on 127.0.0.1:5001...");
                if (app != nullptr)
                {
                    app->netStats.connectionStatus = "CONNECTING";
                    app->netStats.isConnected.store(false);
                }
                continue;
            }

            reconnectAttempts = 0;
            LOGI("[NETWORK] ✓ Connected to Windows receiver at 127.0.0.1:5001");
            SetStatus("Connected to Windows receiver.");
            if (app != nullptr)
            {
                app->netStats.connectionStatus = "CONNECTED";
                app->netStats.isConnected.store(true);
            }

            const bool ok = SendFrameStream(socketFd, app->camera);
            close(socketFd);
            socketFd = -1;

            if (!app->running.load())
            {
                break;
            }

            if (!ok)
            {
                LOGI("[NETWORK] ✗ Connection dropped. Reconnecting...");
                SetStatus("Connection dropped. Reconnecting...");
                if (app != nullptr)
                {
                    app->netStats.connectionStatus = "DISCONNECTED";
                    app->netStats.isConnected.store(false);
                }
            }
        }
        LOGI("[NETWORK] Network loop ended");
    }

    static void HandleAppCmd(android_app* app, int32_t cmd)
    {
        auto* state = static_cast<AppState*>(app->userData);
        switch (cmd)
        {
        case APP_CMD_TERM_WINDOW:
            LOGI("[APP] Window closed");
            SetStatus("Window closed.");
            break;
        case APP_CMD_GAINED_FOCUS:
            LOGI("[APP] Gained focus - checking camera permissions");
            if (state != nullptr && HasCameraPermission(app) && !state->cameraStarted.load())
            {
                LOGI("[APP] Camera permission granted - starting camera");
                if (StartCamera(state->camera))
                {
                    state->cameraStarted.store(true);
                    LOGI("[APP] ✓ Camera started successfully");
                }
                else
                {
                    LOGE("[APP] ✗ Failed to start camera");
                }
            }
            else if (state != nullptr && state->cameraStarted.load())
            {
                LOGI("[APP] Camera already started");
            }
            break;
        case APP_CMD_LOST_FOCUS:
            LOGI("[APP] Lost focus");
            break;
        case APP_CMD_PAUSE:
            LOGI("[APP] App paused");
            break;
        case APP_CMD_RESUME:
            LOGI("[APP] App resumed");
            break;
        default:
            LOGI("[APP] Command: %d", cmd);
            break;
        }
    }
}

void android_main(struct android_app* app)
{
    LOGI("=== HandSightCamera Data Testing App Started ===");
    LOGI("[INIT] App version: 2.0 | Protocol: HSF2v2 | Target: Windows 127.0.0.1:5001");

    AppState state;
    state.app = app;
    state.netStats.lastSampleTime = std::chrono::steady_clock::now();
    g_app = &state;

    app->userData = &state;
    app->onAppCmd = HandleAppCmd;

    LOGI("[INIT] Spawning network thread...");
    state.networkThread = std::thread(NetworkLoop, &state);

    auto lastPermissionCheck = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    auto lastDebugLog = std::chrono::steady_clock::now();

    LOGI("[INIT] Entering main loop - waiting for events");

    while (state.running.load())
    {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(16, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0)
        {
            if (source != nullptr)
            {
                source->process(app, source);
            }
            if (app->destroyRequested)
            {
                LOGI("[MAIN] Destroy requested - shutting down");
                state.running.store(false);
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();

        if (now - lastPermissionCheck >= std::chrono::seconds(1))
        {
            if (HasCameraPermission(app) && !state.cameraStarted.load())
            {
                LOGI("[MAIN] Camera permission detected - starting camera");
                SetStatus("Camera permission granted. Starting camera...");
                if (StartCamera(state.camera))
                {
                    state.cameraStarted.store(true);
                    LOGI("[MAIN] ✓ Camera started");
                }
                else
                {
                    LOGE("[MAIN] ✗ Camera failed to start");
                    SetStatus("Camera failed to start.");
                }
            }
            lastPermissionCheck = now;
        }

        if (now - lastDebugLog >= std::chrono::seconds(5) && state.cameraStarted.load())
        {
            LOGI("[MAIN] === DEBUG STATS ===");
            LOGI("[MAIN] Camera: %llu frames captured | Network: %llu frames sent | Connected: %s",
                 static_cast<unsigned long long>(state.camera.capturedFrames.load()),
                 static_cast<unsigned long long>(state.camera.sentFrames.load()),
                 state.netStats.isConnected.load() ? "YES" : "NO");
            if (state.netStats.isConnected.load())
            {
                LOGI("[MAIN] Throughput: %.2f MB/s | Frame rate: %.1f FPS",
                     state.netStats.currentBytesSec / (1024.0 * 1024.0),
                     state.netStats.currentFramesSec);
            }
            lastDebugLog = now;
        }
    }

    LOGI("[SHUTDOWN] Stopping camera...");
    state.running.store(false);
    if (state.cameraStarted.load())
    {
        StopCamera(state.camera);
        LOGI("[SHUTDOWN] Camera stopped");
    }

    LOGI("[SHUTDOWN] Notifying network thread...");
    state.camera.queueCv.notify_all();
    if (state.networkThread.joinable())
    {
        LOGI("[SHUTDOWN] Waiting for network thread to finish...");
        state.networkThread.join();
        LOGI("[SHUTDOWN] Network thread finished");
    }

    LOGI("[SHUTDOWN] Final stats - Captured: %llu | Sent: %llu | Total bytes: %.2f MB",
         static_cast<unsigned long long>(state.camera.capturedFrames.load()),
         static_cast<unsigned long long>(state.camera.sentFrames.load()),
         state.netStats.totalBytesSent / (1024.0 * 1024.0));
    LOGI("=== HandSightCamera Shutdown Complete ===");

    g_app = nullptr;
}
