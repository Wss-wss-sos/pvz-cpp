@echo off
chcp 65001 >nul
REM ============================================================
REM  植物大战僵尸 C++ 版 - 编译脚本
REM  需要任选其一：
REM    1) MinGW-w64 (g++)      -> 确保 g++ 在 PATH 中
REM    2) Visual Studio (cl)   -> 在 "开发人员命令提示符" 中运行
REM ============================================================
cd /d "%~dp0"

where g++ >nul 2>nul
if %errorlevel%==0 (
    echo [MinGW-w64] 正在编译...
    REM -municode : 宽字符入口点 wWinMain（源码定义了 UNICODE）
    REM -static   : 静态链接运行时，生成独立 exe（无需随附 DLL）
    g++ -O2 -std=c++17 -static -municode -o pvz.exe pvz.cpp -lgdi32 -luser32 -lwinmm -mwindows
    if %errorlevel%==0 (
        echo [完成] pvz.exe 生成成功，双击即可运行
        echo         pvz.exe --selftest            运行无头自检
        echo         pvz.exe --shot out.bmp 1      渲染战斗画面并存为 BMP
    ) else (
        echo [失败] 编译出错，请检查上方错误信息
    )
    goto end
)

where cl >nul 2>nul
if %errorlevel%==0 (
    echo [MSVC] 正在编译...
    cl /nologo /O2 /EHsc /std:c++17 /DUNICODE /D_UNICODE pvz.cpp user32.lib gdi32.lib winmm.lib
    if %errorlevel%==0 (
        echo [完成] pvz.exe 生成成功，双击即可运行
    ) else (
        echo [失败] 编译出错，请检查上方错误信息
    )
    goto end
)

echo [错误] 未找到编译器。
echo 请安装 MinGW-w64 或 Visual Studio，并确保其在 PATH 中。

:end
pause
