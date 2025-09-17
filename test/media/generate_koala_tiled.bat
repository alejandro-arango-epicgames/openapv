REM Generate yuv
REM
REM It generates a single .yuv with all the 455 frames(@ 30 fps) of the 
REM source footage. It is encoded in yuv422p10, so each pixel is a 10-bit 
REM luma and a 10-bit chroma, but padded in 16-bit values.

@echo off

set NAME=koala
set INPUT=%NAME%.mp4
set YUV=%NAME%_3840x2160_yuv422p10le.yuv
set NUMFRAMES=10

ffmpeg -y -i "%INPUT%" -pix_fmt yuv422p10le -s 3840x2160 -f rawvideo -frames:v %NUMFRAMES% "%YUV%"

REM Generate .apv1 frames

..\..\build\bin\release\oapv_app_enc -i "%YUV%" -w 3840 -h 2160 -d 10 -z 30 --input-csp 2 --tile-w 256 --tile-h 256 --tmv-mips -o %NAME%_tiled/%NAME%_tiled



