@echo off
setlocal

echo Inicializando ambiente MSVC (x86)...
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul

echo.
echo Gerando estrutura do CMake (x86 Win32)...
cmake -B build -A Win32

echo.
echo Compilando Launcher e Legacy.dll em Paralelo (Multi-Thread)...
cmake --build build --config Release --parallel
