@echo off
REM Quick test runner - runs only the essential tests without cleaning

echo Quick OpenAPV Test
echo ==================
echo.

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

REM Run the quick tests

set TEST=multi_full_frame_validation
set FRAME=media\koala_16k_tiled\koala_16k_tiled_0000.apv1

echo Running %TEST% test...
..\build\Release\decoder_test.exe %FRAME% %TEST%

if errorlevel 1 goto error
echo.


echo Test complete!
goto end

:error
echo ERROR: Test failed!
exit /b 1

:end