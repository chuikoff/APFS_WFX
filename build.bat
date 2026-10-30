@echo off
echo Building 64-bit plugin...
cmake -B build -S . -A x64
cmake --build build --config Release

echo Building 32-bit plugin...
cmake -B build32 -S . -A Win32
cmake --build build32 --config Release

echo Updating dist...
copy /Y build\Release\apfs.wfx64 dist\apfs.wfx64
copy /Y build32\Release\apfs.wfx dist\apfs.wfx
copy /Y res\apfs.ico dist\apfs.ico

echo Creating zip package...
powershell -Command "Compress-Archive -Path dist\* -DestinationPath tc_apfs.zip -Force"

echo Build completed successfully!
