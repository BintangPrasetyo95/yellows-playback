# Yellow's Playback

A lightweight, transparent desktop overlay for displaying current media playback information (Song Title, Artist, Album Art, and a Progress Bar).

## Building

This project requires the Microsoft Visual C++ (MSVC) compiler and the Windows SDK. To compile the project, open the **x64 Native Tools Command Prompt for VS** (or an equivalent developer prompt where `cl.exe` is available) and run:

```cmd
cl /EHsc /std:c++20 main.cpp /link /out:visualizer.exe
```

## Running

Once compiled, simply run the generated executable:

```cmd
visualizer.exe
```

A console window will spawn for debug output, and the transparent overlay will appear on your screen, ready to fetch media updates.
