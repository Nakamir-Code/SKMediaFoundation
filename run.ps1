# From the project root directory
$config = 'Debug' # Debug/Release

# Make a folder to build in and ignore if it's already there
mkdir build -ErrorAction SilentlyContinue
cd build

# Configure the build
cmake .. -DCMAKE_BUILD_TYPE=$config
# Build
cmake --build . -j8 --config $config

cd ..

# Run the app
& .\build\$config\SKVideoDecoder