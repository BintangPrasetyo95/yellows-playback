#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include <windows.h>
#include <shlwapi.h>
#include <gdiplus.h>
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

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            HBRUSH bgBrush = CreateSolidBrush(RGB(255, 0, 255));
            FillRect(hdc, &ps.rcPaint, bgBrush);
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

            RECT titleRect = { 147, 36, 368, 60 };
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFontTitle);
            DrawTextW(hdc, title.c_str(), -1, &titleRect, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT artistRect = { 147, 57, 368, 100 };
            SelectObject(hdc, hFontArtist);
            DrawTextW(hdc, artist.c_str(), -1, &artistRect, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(hdc, hOldFont);
            DeleteObject(hFontTitle);
            DeleteObject(hFontArtist);

            // Gray close button
            HBRUSH grayBrush = CreateSolidBrush(RGB(50, 50, 50));
            RECT closeRect = { 333, 19, 371, 32 };
            FillRect(hdc, &closeRect, grayBrush);
            DeleteObject(grayBrush);

            HPEN whitePen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HPEN tempPen = (HPEN)SelectObject(hdc, whitePen);

            MoveToEx(hdc, 348, 21, NULL);
            LineTo(hdc, 356, 29);
            MoveToEx(hdc, 356, 21, NULL);
            LineTo(hdc, 348, 29);

            SelectObject(hdc, tempPen);
            DeleteObject(whitePen);

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
            RECT btnPrevRect = { 147, 117, 209, 139 };
            FillRect(hdc, &btnPrevRect, btnBrush);
            POINT prevTri[] = { {162, 127}, {162, 137}, {154, 132} };
            Polygon(hdc, prevTri, 3);
            RECT prevBar = { 150, 127, 152, 137 };
            FillRect(hdc, &prevBar, ctrlFgBrush);

            // Play/Pause Button
            RECT btnPlayRect = { 227, 117, 289, 139 };
            FillRect(hdc, &btnPlayRect, btnBrush);
            if (isPlaying) {
                // Draw Pause
                RECT pauseBar1 = { 248, 127, 251, 137 };
                RECT pauseBar2 = { 254, 127, 257, 137 };
                FillRect(hdc, &pauseBar1, ctrlFgBrush);
                FillRect(hdc, &pauseBar2, ctrlFgBrush);
            } else {
                // Draw Play
                POINT playTri[] = { {248, 127}, {248, 137}, {256, 132} };
                Polygon(hdc, playTri, 3);
            }

            // Next Button
            RECT btnNextRect = { 307, 117, 369, 139 };
            FillRect(hdc, &btnNextRect, btnBrush);
            POINT nextTri[] = { {342, 127}, {342, 137}, {350, 132} };
            Polygon(hdc, nextTri, 3);
            RECT nextBar = { 352, 127, 354, 137 };
            FillRect(hdc, &nextBar, ctrlFgBrush);

            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            
            DeleteObject(btnBrush);
            DeleteObject(ctrlFgBrush);
            DeleteObject(nullPen);

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

    // Load custom font
    AddFontResourceExW(L"./assets/fonts/determination/determination.ttf", FR_PRIVATE, 0);

    const wchar_t CLASS_NAME[] = L"VisualizerOverlay";

    WNDCLASS wc = { };
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
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

    RemoveFontResourceExW(L"./assets/fonts/determination/determination.ttf", FR_PRIVATE, 0);

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}