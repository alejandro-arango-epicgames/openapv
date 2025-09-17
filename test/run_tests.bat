@echo off
REM OpenAPV Test Runner
REM Runs ALL decoder tests and generates PNG visualizations

echo ========================================
echo OpenAPV Test Runner
echo ========================================
echo.

REM Check if test media exists
if not exist media\koala_tiled\koala_tiled_0000.apv1 (
    echo ERROR: Test media not found!
    echo Expected: media\koala_tiled\koala_tiled_0000.apv1
    exit /b 1
)

REM Create output directory if it doesn't exist
if not exist output mkdir output

REM Clean previous test outputs
echo Cleaning previous test outputs...
del /Q output\*.raw 2>nul
del /Q output\*.y4m 2>nul
del /Q output\*.png 2>nul
echo.

REM Build the tests
echo Building decoder tests...
cmake --build ..\build --config Release --target decoder_test
if errorlevel 1 (
    echo ERROR: Build failed!
    exit /b 1
)
echo.

REM Run ALL decoder tests
echo ========================================
echo Running ALL decoder tests...
echo ========================================
..\build\Release\decoder_test.exe media\koala_tiled\koala_tiled_0000.apv1 all
if errorlevel 1 (
    echo ERROR: Decoder tests failed!
    exit /b 1
)
echo.


REM Convert all raw files to PNG
echo ========================================
echo Converting all RAW files to PNG images...
echo ========================================
setlocal enabledelayedexpansion
set count=0
for %%f in (output\*.raw) do (
    set /a count+=1
    echo [!count!] Converting %%~nf.raw to PNG...
    python src\convert_tile_to_png.py "%%f"
    if errorlevel 1 (
        echo WARNING: Failed to convert %%f
    )
)
echo Total files converted: %count%
endlocal
echo.

REM Generate summary
echo ========================================
echo Test Summary
echo ========================================
echo.
echo Generated outputs:
for %%e in (raw png y4m) do (
    set count=0
    for %%f in (output\*.%%e) do set /a count+=1
    if !count! gtr 0 echo   %%e files: !count!
)
echo.

REM Display file list
echo Files generated:
echo ----------------
dir /B output\*.png 2>nul | findstr _rgb.png
echo.

echo ========================================
echo All tests completed successfully!
echo ========================================
echo.
echo To view results:
echo   explorer output
echo.