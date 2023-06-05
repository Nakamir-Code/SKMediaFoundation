# From the project root directory
$plat = 'UWP' # UWP, ''

$buildDir = "build"
if ($plat) {
    $buildDir += "-$plat"
}

# Make a folder to build in and ignore if it's already there
mkdir $buildDir -ErrorAction SilentlyContinue
Set-Location $buildDir

if ($plat -eq 'UWP') {
    & cmake '-DCMAKE_CXX_FLAGS=/MP' '-DCMAKE_SYSTEM_NAME=WindowsStore' '-DCMAKE_SYSTEM_VERSION=10.0' '-DDYNAMIC_LOADER=OFF' '-Wno-deprecated' '-Wno-dev' ..
} else {
    & cmake '-DCMAKE_CXX_FLAGS=/MP' '-DDYNAMIC_LOADER=OFF' '-Wno-deprecated' '-Wno-dev' ..
}

# Build
cmake --build .. -j8

Start-Process SKVideoDecoder.sln

Set-Location ..