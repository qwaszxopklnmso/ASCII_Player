@echo off
setlocal
call "%~dp0build.bat"
if errorlevel 1 exit /b 1
if not exist "%~dp0dist" mkdir "%~dp0dist"
copy /y "%~dp0build\ASCIIPlayer.exe" "%~dp0dist\ASCIIPlayer.exe" >nul
copy /y "%~dp0README.md" "%~dp0dist\README.md" >nul
if exist "%~dp0docs" xcopy /e /i /y "%~dp0docs" "%~dp0dist\docs" >nul
if exist "%~dp0samples" xcopy /e /i /y "%~dp0samples" "%~dp0dist\samples" >nul

set "FFMPEG="
for /f "delims=" %%i in ('where ffmpeg.exe 2^>nul') do if not defined FFMPEG set "FFMPEG=%%i"
if not defined FFMPEG if exist "E:\ffmpeg-8.1.2-essentials_build\bin\ffmpeg.exe" set "FFMPEG=E:\ffmpeg-8.1.2-essentials_build\bin\ffmpeg.exe"
if not defined FFMPEG if exist "%~dp0ffmpeg.exe" set "FFMPEG=%~dp0ffmpeg.exe"
if defined FFMPEG (
    copy /y "%FFMPEG%" "%~dp0dist\ffmpeg.exe" >nul
    for %%d in ("%FFMPEG%") do (
        if exist "%%~dpd..\LICENSE" copy /y "%%~dpd..\LICENSE" "%~dp0dist\FFMPEG_LICENSE.txt" >nul
    )
    echo Bundled FFmpeg: %FFMPEG%
) else (
    echo WARNING: ffmpeg.exe not found; dist will need FFmpeg on PATH.
)
echo.
echo Packaged: %~dp0dist\ASCIIPlayer.exe
