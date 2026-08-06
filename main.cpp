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
const int WINDOW_HEIGHT = 150;

std::mutex g_mutex;
std::wstring g_songTitle = L"Waiting for media...";
std::wstring g_songArtist = L"";
HBITMAP g_hCoverImage = NULL;
bool g_running = true;

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
                                    // Scale bitmap smoothly to 100x100
                                    Gdiplus::Bitmap* resized = new Gdiplus::Bitmap(100, 100, pBitmap->GetPixelFormat());
                                    Gdiplus::Graphics* graphics = Gdiplus::Graphics::FromImage(resized);
                                    graphics->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                                    graphics->DrawImage(pBitmap, 0, 0, 100, 100);
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
                }
            } else if (lastTitle != L"") {
                lastTitle = L"";
                std::lock_guard<std::mutex> lock(g_mutex);
                g_songTitle = L"No Media";
                g_songArtist = L"";
                if (g_hCoverImage) {
                    DeleteObject(g_hCoverImage);
                    g_hCoverImage = NULL;
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } catch (...) {
            std::wcout << L"[MediaThread] Silent COM error caught, will retry next tick." << std::endl;
        }
        
        Sleep(1000);
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE:
            return 0;

        case WM_LBUTTONDOWN: {
            int mouseX = LOWORD(lParam);
            int mouseY = HIWORD(lParam);
            
            // Close button bounding box
            if (mouseX >= 370 && mouseX <= 400 && mouseY >= 0 && mouseY <= 30) {
                PostQuitMessage(0);
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

            std::wstring title, artist;
            HBITMAP cover;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                title = g_songTitle;
                artist = g_songArtist;
                cover = g_hCoverImage; // Shallow copy the handle for rendering
            }

            // Draw Cover
            if (cover) {
                HDC hMemDC = CreateCompatibleDC(hdc);
                HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemDC, cover);
                BitBlt(hdc, 20, 25, 100, 100, hMemDC, 0, 0, SRCCOPY);
                SelectObject(hMemDC, hOldBitmap);
                DeleteDC(hMemDC);
            } else {
                HBRUSH placeholder = CreateSolidBrush(RGB(50, 50, 50));
                RECT coverRect = { 20, 25, 120, 125 };
                FillRect(hdc, &coverRect, placeholder);
                DeleteObject(placeholder);
            }

            // Draw Text
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            
            HFONT hFontTitle = CreateFontW(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Determination");
            HFONT hFontArtist = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Determination");

            RECT titleRect = { 135, 30, 360, 60 };
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFontTitle);
            DrawTextW(hdc, title.c_str(), -1, &titleRect, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT artistRect = { 135, 65, 360, 95 };
            SelectObject(hdc, hFontArtist);
            DrawTextW(hdc, artist.c_str(), -1, &artistRect, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(hdc, hOldFont);
            DeleteObject(hFontTitle);
            DeleteObject(hFontArtist);

            // Red close button
            HBRUSH redBrush = CreateSolidBrush(RGB(230, 0, 0));
            RECT closeRect = { 370, 0, 400, 30 };
            FillRect(hdc, &closeRect, redBrush);
            DeleteObject(redBrush);

            HPEN whitePen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HPEN oldPen = (HPEN)SelectObject(hdc, whitePen);

            MoveToEx(hdc, 375, 5, NULL);
            LineTo(hdc, 395, 25);
            MoveToEx(hdc, 395, 5, NULL);
            LineTo(hdc, 375, 25);

            SelectObject(hdc, oldPen);
            DeleteObject(whitePen);

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
    
    // Load custom font
    AddFontResourceExW(L"./assets/fonts/determination/determination.ttf", FR_PRIVATE, 0);

    const char CLASS_NAME[] = "VisualizerOverlay";

    WNDCLASS wc = { };
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    DWORD style = WS_POPUP;

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    
    int posX = (screenWidth - WINDOW_WIDTH) / 2;
    int posY = screenHeight - WINDOW_HEIGHT - 50;

    HWND hwnd = CreateWindowEx(
        exStyle, CLASS_NAME, "Audio Visualizer", style,
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

    RemoveFontResourceExW(L"./assets/fonts/determination/determination.ttf", FR_PRIVATE, 0);

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}