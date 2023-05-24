# To use this template, 
cmake_minimum_required(VERSION 3.11)
project(SKVideoDecoder VERSION "0.1.0" LANGUAGES CXX C)

# Grab and build StereoKit from the GitHub repository. Here we're setting SK up
# as a statically linked library.
include(FetchContent)
set(SK_BUILD_TESTS       OFF CACHE INTERNAL "")
set(SK_BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_Declare(
  StereoKitC
  GIT_REPOSITORY https://github.com/Nakamir-Code/StereoKit.git #https://github.com/StereoKit/StereoKit.git
  GIT_TAG        f0883c4621df70a61bf38ae4cda2ac7b9a39e049
)
FetchContent_MakeAvailable(StereoKitC)

set(NAK_SRC_CODE
	src/error.h

	src/nv12_tex.cpp
	src/nv12_tex.h
	src/nv12_sprite.cpp
	src/nv12_sprite.h
)

# Add source files to our target executable
add_executable( SKVideoDecoder
  src/main.cpp
  ${NAK_SRC_CODE}
)

# Link to dependencies
target_link_libraries( SKVideoDecoder
  PRIVATE StereoKitC
)