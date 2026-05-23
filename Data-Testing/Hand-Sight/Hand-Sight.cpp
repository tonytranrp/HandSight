#include "Hand-Sight.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../shared/HandSightStreamProtocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    template <typename T>
    void SafeRelease(T*& ptr)
    {
        if (ptr != nullptr)
        {
            ptr->Release();
            ptr = nullptr;
        }
    }

    struct FrameBuffer
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> pixels;
        bool valid = false;
        std::mutex mutex;
    };

    struct AppState
    {
        HWND window = nullptr;
        std::atomic<bool> running{ true };
        std::thread networkThread;
        FrameBuffer frame;
        std::wstring status = L"Waiting for Android client...";
        std::mutex statusMutex;
        IWICImagingFactory* wicFactory = nullptr;
    };

    AppState* g_app = nullptr;

    void SetStatus(const std::wstring& text)
    {
        if (g_app == nullptr)
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_app->statusMutex);
            g_app->status = text;
        }

        if (g_app->window != nullptr)
        {
            InvalidateRect(g_app->window, nullptr, FALSE);
        }
    }

    std::wstring GetStatus()
    {
        if (g_app == nullptr)
        {
            return L"";
        }

        std::lock_guard<std::mutex> lock(g_app->statusMutex);
        return g_app->status;
    }

    bool RecvAll(SOCKET socket, void* buffer, int size)
    {
        auto* bytes = static_cast<std::uint8_t*>(buffer);
        int received = 0;
        while (received < size && g_app != nullptr && g_app->running.load())
        {
            const int result = recv(socket, reinterpret_cast<char*>(bytes + received), size - received, 0);
            if (result > 0)
            {
                received += result;
                continue;
            }

            if (result == 0)
            {
                return false;
            }

            const int error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK || error == WSAEINTR)
            {
                continue;
            }

            return false;
        }

        return received == size;
    }

    RECT ComputeFitRect(const RECT& client, std::uint32_t sourceWidth, std::uint32_t sourceHeight)
    {
        RECT result{ client.left, client.top, client.right, client.bottom };
        const int clientWidth = std::max(0L, client.right - client.left);
        const int clientHeight = std::max(0L, client.bottom - client.top);
        if (sourceWidth == 0 || sourceHeight == 0 || clientWidth == 0 || clientHeight == 0)
        {
            return result;
        }

        const double scale = std::min(static_cast<double>(clientWidth) / static_cast<double>(sourceWidth),
                                      static_cast<double>(clientHeight) / static_cast<double>(sourceHeight));
        const int drawWidth = static_cast<int>(std::lround(static_cast<double>(sourceWidth) * scale));
        const int drawHeight = static_cast<int>(std::lround(static_cast<double>(sourceHeight) * scale));

        const int offsetX = client.left + (clientWidth - drawWidth) / 2;
        const int offsetY = client.top + (clientHeight - drawHeight) / 2;
        result.left = offsetX;
        result.top = offsetY;
        result.right = offsetX + drawWidth;
        result.bottom = offsetY + drawHeight;
        return result;
    }

    bool DecodeJpegFrame(IWICImagingFactory* factory, const std::uint8_t* jpegData, size_t jpegSize, std::uint32_t rotationDegrees, FrameBuffer& frame)
    {
        if (factory == nullptr || jpegData == nullptr || jpegSize == 0)
        {
            return false;
        }

        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* sourceFrame = nullptr;
        IWICFormatConverter* converter = nullptr;
        IWICBitmapFlipRotator* rotator = nullptr;

        bool ok = false;
        do
        {
            HRESULT hr = factory->CreateStream(&stream);
            if (FAILED(hr))
            {
                break;
            }

            hr = stream->InitializeFromMemory(const_cast<BYTE*>(jpegData), static_cast<DWORD>(jpegSize));
            if (FAILED(hr))
            {
                break;
            }

            hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
            if (FAILED(hr))
            {
                break;
            }

            hr = decoder->GetFrame(0, &sourceFrame);
            if (FAILED(hr))
            {
                break;
            }

            hr = factory->CreateFormatConverter(&converter);
            if (FAILED(hr))
            {
                break;
            }

            hr = converter->Initialize(
                sourceFrame,
                GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom);
            if (FAILED(hr))
            {
                break;
            }

            IWICBitmapSource* source = converter;
            if (rotationDegrees == 90 || rotationDegrees == 180 || rotationDegrees == 270)
            {
                WICBitmapTransformOptions transform = WICBitmapTransformRotate0;
                if (rotationDegrees == 90)
                {
                    transform = WICBitmapTransformRotate90;
                }
                else if (rotationDegrees == 180)
                {
                    transform = WICBitmapTransformRotate180;
                }
                else if (rotationDegrees == 270)
                {
                    transform = WICBitmapTransformRotate270;
                }

                hr = factory->CreateBitmapFlipRotator(&rotator);
                if (FAILED(hr))
                {
                    break;
                }

                hr = rotator->Initialize(converter, transform);
                if (FAILED(hr))
                {
                    break;
                }

                source = rotator;
            }

            UINT width = 0;
            UINT height = 0;
            hr = source->GetSize(&width, &height);
            if (FAILED(hr) || width == 0 || height == 0)
            {
                break;
            }

            const UINT stride = width * 4;
            std::vector<std::uint8_t> pixels(static_cast<size_t>(stride) * static_cast<size_t>(height));
            hr = source->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
            if (FAILED(hr))
            {
                break;
            }

            frame.width = width;
            frame.height = height;
            frame.pixels = std::move(pixels);
            frame.valid = true;
            ok = true;
        } while (false);

        SafeRelease(rotator);
        SafeRelease(converter);
        SafeRelease(sourceFrame);
        SafeRelease(decoder);
        SafeRelease(stream);
        return ok;
    }

    bool ReceiveStream(SOCKET socket)
    {
        handsight::StreamHeader header{};
        if (!RecvAll(socket, &header, sizeof(header)))
        {
            return false;
        }

        if (header.magic != handsight::kStreamMagic || header.version != handsight::kStreamVersion || header.pixelFormat != handsight::kPixelFormatJpeg)
        {
            SetStatus(L"Unsupported stream header.");
            return false;
        }

        SetStatus(L"Android stream connected.");

        std::vector<std::uint8_t> frameBytes;
        std::vector<std::uint8_t> nextFrameBytes;
        bool hasNextFrame = false;
        std::uint64_t frameCount = 0;

        while (g_app != nullptr && g_app->running.load())
        {
            handsight::FrameHeader frameHeader{};
            if (!RecvAll(socket, &frameHeader, sizeof(frameHeader)))
            {
                return false;
            }

            if (frameHeader.payloadSize == 0 || frameHeader.payloadSize > 32u * 1024u * 1024u)
            {
                SetStatus(L"Invalid JPEG payload.");
                return false;
            }

            nextFrameBytes.resize(frameHeader.payloadSize);
            if (!RecvAll(socket, nextFrameBytes.data(), static_cast<int>(nextFrameBytes.size())))
            {
                return false;
            }

            if (!hasNextFrame)
            {
                frameBytes = nextFrameBytes;
                hasNextFrame = true;
            }
            else
            {
                frameBytes = nextFrameBytes;
            }

            FrameBuffer decodedFrame;
            if (!DecodeJpegFrame(g_app->wicFactory, frameBytes.data(), frameBytes.size(), header.rotationDegrees, decodedFrame))
            {
                SetStatus(L"JPEG decode failed.");
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(g_app->frame.mutex);
                g_app->frame.width = decodedFrame.width;
                g_app->frame.height = decodedFrame.height;
                g_app->frame.pixels = std::move(decodedFrame.pixels);
                g_app->frame.valid = true;
            }

            if (g_app->window != nullptr)
            {
                InvalidateRect(g_app->window, nullptr, FALSE);
            }

            frameCount++;
        }

        return false;
    }

    void PaintFrame(HWND hwnd, HDC hdc)
    {
        RECT client{};
        GetClientRect(hwnd, &client);

        FrameBuffer snapshot;
        {
            std::lock_guard<std::mutex> lock(g_app->frame.mutex);
            snapshot.width = g_app->frame.width;
            snapshot.height = g_app->frame.height;
            snapshot.pixels = g_app->frame.pixels;
            snapshot.valid = g_app->frame.valid;
        }

        if (!snapshot.valid || snapshot.pixels.empty())
        {
            HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
            if (background != nullptr)
            {
                FillRect(hdc, &client, background);
                DeleteObject(background);
            }

            const std::wstring status = GetStatus();
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(240, 240, 240));
            DrawTextW(hdc, status.c_str(), -1, &client, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            return;
        }

        const RECT fit = ComputeFitRect(client, snapshot.width, snapshot.height);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(snapshot.width);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(snapshot.height);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(
            hdc,
            fit.left,
            fit.top,
            fit.right - fit.left,
            fit.bottom - fit.top,
            0,
            0,
            static_cast<int>(snapshot.width),
            static_cast<int>(snapshot.height),
            snapshot.pixels.data(),
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY);
    }

    void NetworkThread()
    {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            SetStatus(L"WSAStartup failed.");
            return;
        }

        const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(coInit))
        {
            SetStatus(L"COM initialization failed.");
            WSACleanup();
            return;
        }

        const HRESULT factoryResult = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_app->wicFactory));
        if (FAILED(factoryResult))
        {
            SetStatus(L"Failed to create WIC factory.");
            CoUninitialize();
            WSACleanup();
            return;
        }

        SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET)
        {
            SetStatus(L"Failed to create listen socket.");
            SafeRelease(g_app->wicFactory);
            CoUninitialize();
            WSACleanup();
            return;
        }

        BOOL reuse = TRUE;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(5001);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
        {
            SetStatus(L"Failed to bind port 5001.");
            closesocket(listenSocket);
            SafeRelease(g_app->wicFactory);
            CoUninitialize();
            WSACleanup();
            return;
        }

        if (listen(listenSocket, 1) == SOCKET_ERROR)
        {
            SetStatus(L"Failed to listen on port 5001.");
            closesocket(listenSocket);
            SafeRelease(g_app->wicFactory);
            CoUninitialize();
            WSACleanup();
            return;
        }

        SetStatus(L"Waiting for adb reverse client on 127.0.0.1:5001...");

        while (g_app != nullptr && g_app->running.load())
        {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSocket, &readSet);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 250000;

            const int ready = select(0, &readSet, nullptr, nullptr, &tv);
            if (ready <= 0)
            {
                continue;
            }

            sockaddr_in clientAddr{};
            int clientSize = sizeof(clientAddr);
            SOCKET clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientSize);
            if (clientSocket == INVALID_SOCKET)
            {
                continue;
            }

            const int tcpNoDelay = 1;
            setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&tcpNoDelay), sizeof(tcpNoDelay));

            DWORD timeoutMs = 1000;
            setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
            SetStatus(L"Client connected. Receiving JPEG frames...");

            const bool keepReceiving = ReceiveStream(clientSocket);
            closesocket(clientSocket);

            if (!g_app->running.load())
            {
                break;
            }

            if (!keepReceiving)
            {
                SetStatus(L"Connection lost. Waiting for reconnect...");
            }
        }

        closesocket(listenSocket);
        SafeRelease(g_app->wicFactory);
        CoUninitialize();
        WSACleanup();
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintFrame(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            if (g_app != nullptr)
            {
                g_app->running.store(false);
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    }
}

int main()
{
    AppState app;
    g_app = &app;

    const wchar_t className[] = L"HandSightReceiverWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc))
    {
        MessageBoxW(nullptr, L"Failed to register window class.", L"HandSight", MB_ICONERROR);
        return 1;
    }

    app.window = CreateWindowExW(
        0,
        className,
        L"HandSight - Android Live Feed",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1280,
        720,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);

    if (!app.window)
    {
        MessageBoxW(nullptr, L"Failed to create the receiver window.", L"HandSight", MB_ICONERROR);
        g_app = nullptr;
        return 1;
    }

    ShowWindow(app.window, SW_SHOW);
    UpdateWindow(app.window);

    app.networkThread = std::thread(NetworkThread);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app.running.store(false);
    if (app.networkThread.joinable())
    {
        app.networkThread.join();
    }

    SafeRelease(app.wicFactory);
    g_app = nullptr;
    return static_cast<int>(msg.wParam);
}
