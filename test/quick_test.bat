@echo off
REM Quick test runner - runs only the essential tests without cleaning

echo Quick OpenAPV Test
echo ==================
echo.

REM Run the main 6x4 multi-tile test
echo Running 6x4 multi-tile test...
..\build\Release\decoder_test.exe media\koala_tiled\koala_tiled_0000.apv1 multi_middle_6x4_scaling
if errorlevel 1 goto error
echo.

REM Convert the output to PNG
if exist output\multi_middle_6x4_scaling.raw (
    echo Converting to PNG...
    python src\convert_tile_to_png.py output\multi_middle_6x4_scaling.raw
    if errorlevel 1 echo WARNING: PNG conversion failed
    echo.
    
    REM Open the result
    if exist output\multi_middle_6x4_scaling_rgb.png (
        echo Opening result...
        start output\multi_middle_6x4_scaling_rgb.png
    )
)

echo Test complete!
goto end

:error
echo ERROR: Test failed!
exit /b 1

:end