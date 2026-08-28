#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

#include <windows.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <iostream>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;

const int WINDOW_WIDTH = 400;
const int WINDOW_HEIGHT = 176;

std::mutex g_mutex;
std::wstring g_songTitle = L"Waiting for media...";
std::wstring g_songArtist = L"";
HBITMAP g_hCoverImage = NULL;
bool g_running = true;
double g_songProgress = 0.0;
bool g_isPlaying = false;
Gdiplus::Image* g_imgBackground = nullptr;
Gdiplus::Image* g_imgPrev = nullptr;
Gdiplus::Image* g_imgPlay = nullptr;
Gdiplus::Image* g_imgPause = nullptr;
Gdiplus::Image* g_imgNext = nullptr;
Gdiplus::Image* g_imgSoundOn = nullptr;
Gdiplus::Image* g_imgSoundOff = nullptr;
Gdiplus::Image* g_imgPinOn = nullptr;
Gdiplus::Image* g_imgPinOff = nullptr;
Gdiplus::Image* g_imgClose = nullptr;
bool g_isPinned = true;
bool g_isMuted = false;

void SendMediaKey(WORD vk) {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

// This function runs in the background and constantly updates memory with the current song!
void FetchMediaLoop(HWND hwnd) {
    // Initialize the COM apartment for this thread
    init_apartment();
    
    std::wcout << L"[MediaThread] Requesting media session manager..." << std::endl;
    auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    std::wstring lastTitle = L"";

    while (g_running) {
        try {
            auto session = manager.GetCurrentSession();
            if (session) {
                auto timeline = session.GetTimelineProperties();
                double currentProgress = 0.0;
                if (timeline) {
                    auto pos = timeline.Position().count();
                    auto end = timeline.EndTime().count();
                    if (end > 0) {
                        currentProgress = (double)pos / end;
                        if (currentProgress > 1.0) currentProgress = 1.0;
                        if (currentProgress < 0.0) currentProgress = 0.0;
                    }
                }

                auto playbackInfo = session.GetPlaybackInfo();
                bool currentIsPlaying = false;
                if (playbackInfo) {
                    currentIsPlaying = (playbackInfo.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                }

                bool currentIsMuted = false;
                IMMDeviceEnumerator* pEnum = NULL;
                if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void**)&pEnum))) {
                    IMMDevice* pDevice = NULL;
                    if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice))) {
                        IAudioEndpointVolume* pVol = NULL;
                        if (SUCCEEDED(pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL, (void**)&pVol))) {
                            BOOL bMute = FALSE;
                            pVol->GetMute(&bMute);
                            currentIsMuted = (bMute != FALSE);
                            pVol->Release();
                        }
                        pDevice->Release();
                    }
                    pEnum->Release();
                }

                bool stateChanged = false;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    if (abs(g_songProgress - currentProgress) > 0.005) {
                        g_songProgress = currentProgress;
                        stateChanged = true;
                    }
                    if (g_isPlaying != currentIsPlaying) {
                        g_isPlaying = currentIsPlaying;
                        stateChanged = true;
                    }
                    if (g_isMuted != currentIsMuted) {
                        g_isMuted = currentIsMuted;
                        stateChanged = true;
                    }
                }

                auto info = session.TryGetMediaPropertiesAsync().get();
                std::wstring currentTitle(info.Title());
                
                if (currentTitle != lastTitle) {
                    lastTitle = currentTitle;
                    
                    std::wstring title(info.Title());
                    std::wstring artist(info.Artist());
                    std::wcout << L"[MediaThread] New song detected: " << title << L" by " << artist << std::endl;
                    
                    HBITMAP newCover = NULL;
                    auto thumbRef = info.Thumbnail();
                    if (thumbRef) {
                        try {
                            auto stream = thumbRef.OpenReadAsync().get();
                            uint32_t size = (uint32_t)stream.Size();
                            DataReader reader(stream);
                            reader.LoadAsync(size).get();
                            std::vector<uint8_t> buffer(size);
                            reader.ReadBytes(buffer);
                            
                            // Create a stream directly from memory for GDI+
                            IStream* pStream = SHCreateMemStream(buffer.data(), (UINT)buffer.size());
                            if (pStream) {
                                Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream);
                                if (pBitmap) {
                                    // Scale bitmap smoothly to 75x75
                                    Gdiplus::Bitmap* resized = new Gdiplus::Bitmap(90, 90, pBitmap->GetPixelFormat());
                                    Gdiplus::Graphics* graphics = Gdiplus::Graphics::FromImage(resized);
                                    graphics->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                                    graphics->DrawImage(pBitmap, 0, 0, 90, 90);
                                    resized->GetHBITMAP(Gdiplus::Color(255, 0, 255), &newCover); // Background color key (magenta)
                                    delete graphics;
                                    delete resized;
                                    delete pBitmap;
                                }
                                pStream->Release();
                            }
                        } catch (...) {
                            // Ignore if cover art fails to load
                        }
                    }
                    
                    // Safely update globals
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_songTitle = title;
                        g_songArtist = artist;
                        if (g_hCoverImage) {
                            DeleteObject(g_hCoverImage);
                        }
                        g_hCoverImage = newCover;
                    }
                    
                    // Tell the window to repaint itself immediately
                    InvalidateRect(hwnd, NULL, FALSE);
                } else if (stateChanged) {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            } else if (lastTitle != L"") {
                lastTitle = L"";
                std::lock_guard<std::mutex> lock(g_mutex);
                g_songTitle = L"No Media";
                g_songArtist = L"";
                g_songProgress = 0.0;
                if (g_hCoverImage) {
                    DeleteObject(g_hCoverImage);
                    g_hCoverImage = NULL;
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } catch (...) {
            std::wcout << L"[MediaThread] Silent COM error caught, will retry next tick." << std::endl;
        }
        
        Sleep(250);
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE:
            return 0;
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                int mouseX = pt.x;
                int mouseY = pt.y;

                bool isHoveringButton = false;
                // Volume button
                if (mouseX >= 249 && mouseX <= 287 && mouseY >= 19 && mouseY <= 32) isHoveringButton = true;
                // Pin button
                if (mouseX >= 291 && mouseX <= 329 && mouseY >= 19 && mouseY <= 32) isHoveringButton = true;
                // Close button
                if (mouseX >= 333 && mouseX <= 371 && mouseY >= 19 && mouseY <= 32) isHoveringButton = true;
                // Prev button
                if (mouseX >= 147 && mouseX <= 209 && mouseY >= 117 && mouseY <= 139) isHoveringButton = true;
                // Play button
                if (mouseX >= 227 && mouseX <= 289 && mouseY >= 117 && mouseY <= 139) isHoveringButton = true;
                // Next button
                if (mouseX >= 307 && mouseX <= 369 && mouseY >= 117 && mouseY <= 139) isHoveringButton = true;

                if (isHoveringButton) {
                    SetCursor(LoadCursor(NULL, IDC_HAND));
                    return TRUE;
                }
            }
            break; // Let DefWindowProc handle the rest
        }

        case WM_LBUTTONDOWN: {
            int mouseX = LOWORD(lParam);
            int mouseY = HIWORD(lParam);
            
            // Volume button bounding box
            if (mouseX >= 249 && mouseX <= 287 && mouseY >= 19 && mouseY <= 32) {
                g_isMuted = !g_isMuted;
                SendMediaKey(VK_VOLUME_MUTE);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            // Pin button bounding box
            if (mouseX >= 291 && mouseX <= 329 && mouseY >= 19 && mouseY <= 32) {
                g_isPinned = !g_isPinned;
                if (g_isPinned) {
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                } else {
                    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            
            // Close button bounding box
            if (mouseX >= 333 && mouseX <= 371 && mouseY >= 19 && mouseY <= 32) {
                PostQuitMessage(0);
                return 0;
            }
            
            // Media Controls bounding boxes
            if (mouseX >= 147 && mouseX <= 209 && mouseY >= 117 && mouseY <= 139) {
                SendMediaKey(VK_MEDIA_PREV_TRACK);
                return 0;
            }
            if (mouseX >= 227 && mouseX <= 289 && mouseY >= 117 && mouseY <= 139) {
                SendMediaKey(VK_MEDIA_PLAY_PAUSE);
                return 0;
            }
            if (mouseX >= 307 && mouseX <= 369 && mouseY >= 117 && mouseY <= 139) {
                SendMediaKey(VK_MEDIA_NEXT_TRACK);
                return 0;
            }
            
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdcWindow = BeginPaint(hwnd, &ps);

            HDC hdc = CreateCompatibleDC(hdcWindow);
            HBITMAP hMemBmp = CreateCompatibleBitmap(hdcWindow, WINDOW_WIDTH, WINDOW_HEIGHT);
            HBITMAP hOldBmp = (HBITMAP)SelectObject(hdc, hMemBmp);

            HBRUSH bgBrush = CreateSolidBrush(RGB(255, 0, 255));
            RECT clientRect = {0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
            FillRect(hdc, &clientRect, bgBrush);
            DeleteObject(bgBrush);

            // Draw custom background image
            if (g_imgBackground) {
                Gdiplus::Graphics graphics(hdc);
                // Force hard edges by disabling interpolation and smoothing
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                
                graphics.DrawImage(g_imgBackground, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
            }

            std::wstring title, artist;
            HBITMAP cover;
            double progress;
            bool isPlaying;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                title = g_songTitle;
                artist = g_songArtist;
                cover = g_hCoverImage; // Shallow copy the handle for rendering
                progress = g_songProgress;
                isPlaying = g_isPlaying;
            }

            // Draw Cover
            if (cover) {
                HDC hMemDC = CreateCompatibleDC(hdc);
                HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemDC, cover);
                BitBlt(hdc, 30, 43, 90, 90, hMemDC, 0, 0, SRCCOPY);
                SelectObject(hMemDC, hOldBitmap);
                DeleteDC(hMemDC);
            } else {
                HBRUSH placeholder = CreateSolidBrush(RGB(50, 50, 50));
                RECT coverRect = { 30, 43, 120, 133 }; // size+original-size, size+original-size
                FillRect(hdc, &coverRect, placeholder);
                DeleteObject(placeholder);
            }

            // Draw Text
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0, 0, 0)); // Black text
            
            HFONT hFontTitle = CreateFontW(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Determination");
            HFONT hFontArtist = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Determination");

            RECT titleRect = { 147, 34, 368, 62 };
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFontTitle);
            DrawTextW(hdc, title.c_str(), -1, &titleRect, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT artistRect = { 147, 57, 368, 100 };
            SelectObject(hdc, hFontArtist);
            DrawTextW(hdc, artist.c_str(), -1, &artistRect, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(hdc, hOldFont);
            DeleteObject(hFontTitle);
            DeleteObject(hFontArtist);

            // Volume Button
            if (g_isMuted) {
                if (g_imgSoundOff) {
                    Gdiplus::Graphics graphics(hdc);
                    graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                    graphics.DrawImage(g_imgSoundOff, 253, 19, 38, 13);
                }
            } else {
                if (g_imgSoundOn) {
                    Gdiplus::Graphics graphics(hdc);
                    graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                    graphics.DrawImage(g_imgSoundOn, 253, 19, 38, 13);
                }
            }

            // Pin Button
            if (g_isPinned) {
                if (g_imgPinOn) {
                    Gdiplus::Graphics graphics(hdc);
                    graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                    graphics.DrawImage(g_imgPinOn, 293, 19, 38, 13);
                }
            } else {
                if (g_imgPinOff) {
                    Gdiplus::Graphics graphics(hdc);
                    graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                    graphics.DrawImage(g_imgPinOff, 293, 19, 38, 13);
                }
            }

            // Close Button
            if (g_imgClose) {
                Gdiplus::Graphics graphics(hdc);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                graphics.DrawImage(g_imgClose, 333, 19, 38, 13);
            }

            // Draw Progress Bar Background
            HBRUSH barBg = CreateSolidBrush(RGB(50, 50, 50));
            RECT barBgRect = { 147, 93, 368, 104 };
            FillRect(hdc, &barBgRect, barBg);
            DeleteObject(barBg);

            // Draw Progress Bar Foreground
            if (progress > 0.0) {
                // The new width of barBgRect is 363 - 141 = 222
                int fillWidth = (int)(221 * progress);
                if (fillWidth > 221) fillWidth = 221;
                HBRUSH barFg = CreateSolidBrush(RGB(255, 255, 255)); // Now drawn over the new position
                RECT barFgRect = { 147, 93, 147 + fillWidth, 104 };
                FillRect(hdc, &barFgRect, barFg);
                DeleteObject(barFg);
            }

            // Draw Media Controls
            HBRUSH btnBrush = CreateSolidBrush(RGB(40, 40, 40));
            HBRUSH ctrlFgBrush = CreateSolidBrush(RGB(200, 200, 200));
            HPEN nullPen = CreatePen(PS_NULL, 0, 0);
            
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, ctrlFgBrush);
            HPEN oldPen = (HPEN)SelectObject(hdc, nullPen); // Save oldPen here now

            // Prev Button
            if (g_imgPrev) {
                Gdiplus::Graphics graphics(hdc);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                graphics.DrawImage(g_imgPrev, 144, 115, 67, 26);
            }

            // Play/Pause Button
            if (isPlaying) {
                if (g_imgPause) {
                    Gdiplus::Graphics graphics(hdc);
                    graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                    graphics.DrawImage(g_imgPause, 224, 115, 67, 26);
                }
            } else {
                if (g_imgPlay) {
                    Gdiplus::Graphics graphics(hdc);
                    graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                    graphics.DrawImage(g_imgPlay, 224, 115, 67, 26);
                }
            }

            // Next Button
            if (g_imgNext) {
                Gdiplus::Graphics graphics(hdc);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                graphics.DrawImage(g_imgNext, 304, 115, 67, 26);
            }

            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            
            DeleteObject(btnBrush);
            DeleteObject(ctrlFgBrush);
            DeleteObject(nullPen);

            BitBlt(hdcWindow, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, hdc, 0, 0, SRCCOPY);
            SelectObject(hdc, hOldBmp);
            DeleteObject(hMemBmp);
            DeleteDC(hdc);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Allocate a console for debugging
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    std::wcout << L"=== Visualizer Debug Console ===" << std::endl;

    // Initialize GDI+
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    
    // Load images
    g_imgBackground = new Gdiplus::Image(L"./assets/components/background.png");
    g_imgPrev = new Gdiplus::Image(L"./assets/components/prev.png");
    g_imgPlay = new Gdiplus::Image(L"./assets/components/play.png");
    g_imgPause = new Gdiplus::Image(L"./assets/components/pause.png");
    g_imgNext = new Gdiplus::Image(L"./assets/components/next.png");
    g_imgSoundOn = new Gdiplus::Image(L"./assets/components/sound_on.png");
    g_imgSoundOff = new Gdiplus::Image(L"./assets/components/sound_off.png");
    g_imgPinOn = new Gdiplus::Image(L"./assets/components/pin_on.png");
    g_imgPinOff = new Gdiplus::Image(L"./assets/components/pin_off.png");
    g_imgClose = new Gdiplus::Image(L"./assets/components/close.png");

    // Load custom font
    AddFontResourceExW(L"./assets/fonts/determination/determination.ttf", FR_PRIVATE, 0);

    const wchar_t CLASS_NAME[] = L"VisualizerOverlay";

    WNDCLASS wc = { };
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_APPWINDOW;
    DWORD style = WS_POPUP;

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    
    int posX = (screenWidth - WINDOW_WIDTH) / 2;
    int posY = screenHeight - WINDOW_HEIGHT - 50;

    HWND hwnd = CreateWindowEx(
        exStyle, CLASS_NAME, L"Audio Visualizer", style,
        posX, posY, WINDOW_WIDTH, WINDOW_HEIGHT,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) return 0;

    SetLayeredWindowAttributes(hwnd, RGB(255, 0, 255), 0, LWA_COLORKEY);
    ShowWindow(hwnd, nCmdShow);

    // Start background thread for media updates
    std::thread mediaThread(FetchMediaLoop, hwnd);

    MSG msg = { };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = false;
    mediaThread.join();

    if (g_hCoverImage) {
        DeleteObject(g_hCoverImage);
    }

    if (g_imgBackground) {
        delete g_imgBackground;
    }
    if (g_imgPrev) {
        delete g_imgPrev;
    }
    if (g_imgPlay) {
        delete g_imgPlay;
    }
    if (g_imgPause) {
        delete g_imgPause;
    }
    if (g_imgNext) {
        delete g_imgNext;
    }
    if (g_imgSoundOn) delete g_imgSoundOn;
    if (g_imgSoundOff) delete g_imgSoundOff;
    if (g_imgPinOn) delete g_imgPinOn;
    if (g_imgPinOff) delete g_imgPinOff;
    if (g_imgClose) delete g_imgClose;

    RemoveFontResourceExW(L"./assets/fonts/determination/determination.ttf", FR_PRIVATE, 0);

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}