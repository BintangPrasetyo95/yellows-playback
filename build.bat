call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /EHsc /std:c++20 /D UNICODE /D _UNICODE main.cpp /link /out:yellows-playback.exe
