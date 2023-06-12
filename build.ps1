# From the project root directory
$arch = 'x64' #x64, ARM, ARM64
#$plat = 'UWP'

$buildDir = "build"
if ($plat) {
    $buildDir += "-$plat"
}
$buildDir += "-$arch"

# Make a folder to build in and ignore if it's already there
mkdir $buildDir -ErrorAction SilentlyContinue
Set-Location $buildDir

if ($plat -eq 'UWP') {
    & cmake -A $arch '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_CXX_FLAGS=/MP' '-DCMAKE_SYSTEM_NAME=WindowsStore' '-DCMAKE_SYSTEM_VERSION=10.0' '-DDYNAMIC_LOADER=OFF' '-Wno-deprecated' '-Wno-dev' ..
} else {
    & cmake -A $arch '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_CXX_FLAGS=/MP' '-DDYNAMIC_LOADER=OFF' '-Wno-deprecated' '-Wno-dev' ..
}

# Build
& cmake --build .. -j8 --config Release

Start-Process SKMediaFoundation.sln

Set-Location ..