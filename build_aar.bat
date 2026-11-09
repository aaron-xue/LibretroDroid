@echo off
setlocal enabledelayedexpansion

echo ========================================
echo  LibretroDroid AAR Build Script
echo ========================================
echo.

REM Get the directory where this script is located
set "PROJECT_DIR=%~dp0"
cd /d "%PROJECT_DIR%"

REM =============================================
REM 1. Check/Set Android SDK
REM =============================================
if not defined ANDROID_HOME (
    if not defined ANDROID_SDK_ROOT (
        echo [ERROR] ANDROID_HOME or ANDROID_SDK_ROOT is not set.
        echo Please set it to your Android SDK path, for example:
        echo   set ANDROID_HOME=C:\Users\YourName\AppData\Local\Android\Sdk
        echo.
        REM Try common default paths
        if exist "%LOCALAPPDATA%\Android\Sdk" (
            set "ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk"
            echo [INFO] Auto-detected ANDROID_HOME: !ANDROID_HOME!
        ) else if exist "C:\Android\Sdk" (
            set "ANDROID_HOME=C:\Android\Sdk"
            echo [INFO] Auto-detected ANDROID_HOME: !ANDROID_HOME!
        ) else (
            echo [ERROR] Cannot auto-detect Android SDK. Please set ANDROID_HOME manually.
            pause
            exit /b 1
        )
    ) else (
        set "ANDROID_HOME=%ANDROID_SDK_ROOT%"
    )
)

echo [INFO] ANDROID_HOME = %ANDROID_HOME%
echo.

REM Check for NDK (required for CMake native builds)
if not exist "%ANDROID_HOME%\ndk" (
    echo [WARNING] NDK not found in %ANDROID_HOME%\ndk
    echo Native C++ code will NOT compile without NDK.
    echo Please install NDK via Android Studio SDK Manager.
)

REM =============================================
REM 2. Initialize Git Submodules
REM =============================================
echo [STEP] Initializing git submodules...
echo.

git submodule update --init --recursive
if %ERRORLEVEL% neq 0 (
    echo [WARNING] git submodule update failed. Trying to continue anyway...
    echo Make sure git is installed and the submodule URLs are accessible.
)
echo.

REM =============================================
REM 3. Clean previous builds (optional, comment out to skip)
REM =============================================
echo [STEP] Cleaning previous build artifacts...
call gradlew.bat clean
echo.

REM =============================================
REM 4. Build libretrodroid AAR
REM =============================================
echo [STEP] Building libretrodroid-release.aar...
echo This may take a while on first build (downloading dependencies + native compilation)...
echo.

call gradlew.bat :libretrodroid:assembleRelease
if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Build failed! Check the output above for details.
    pause
    exit /b 1
)

echo.
echo ========================================
echo  Build completed successfully!
echo ========================================

REM =============================================
REM 5. Locate and copy the AAR
REM =============================================
set "AAR_SRC=%PROJECT_DIR%libretrodroid\build\outputs\aar\libretrodroid-release.aar"
set "AAR_DST=%PROJECT_DIR%build\libretrodroid.aar"

if exist "%AAR_SRC%" (
    if not exist "%PROJECT_DIR%build" mkdir "%PROJECT_DIR%build"
    copy /y "%AAR_SRC%" "%AAR_DST%" >nul
    echo.
    echo AAR copied to: %AAR_DST%
    for %%A in ("%AAR_SRC%") do echo File size: %%~zA bytes
    echo.
    echo Full path: %AAR_SRC%
) else (
    echo.
    echo [WARNING] AAR not found at expected location: %AAR_SRC%
    echo The build may have succeeded but output is in a different location.
)

echo.
echo ========================================
echo  Done!
echo ========================================
pause
endlocal