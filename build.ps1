# From the project root directory
$plat = 'UWP' # UWP, ''
$arch = 'x64' #x64, ARM

$buildDir = "build"
if ($plat) {
    $buildDir += "-$plat"
}
$buildDir += "-$arch"

# Make a folder to build in and ignore if it's already there
mkdir $buildDir -ErrorAction SilentlyContinue
Set-Location $buildDir

if ($plat -eq 'UWP') {
    & cmake -A $arch '-DCMAKE_CXX_FLAGS=/MP' '-DCMAKE_SYSTEM_NAME=WindowsStore' '-DCMAKE_SYSTEM_VERSION=10.0' '-DDYNAMIC_LOADER=OFF' '-Wno-deprecated' '-Wno-dev' ..
} else {
    & cmake -A $arch '-DCMAKE_CXX_FLAGS=/MP' '-DDYNAMIC_LOADER=OFF' '-Wno-deprecated' '-Wno-dev' ..
}

# Build
cmake --build .. -j8

Start-Process SKVideoDecoder.sln

Set-Location ..