# To use this template, 
cmake_minimum_required(VERSION 3.11)
project(SKMediaFoundation VERSION "0.1.0" LANGUAGES CXX C)
set(CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)

# Grab and build StereoKit from the GitHub repository. Here we're setting SK up
# as a statically linked library.
include(FetchContent)
set(SK_BUILD_TESTS       OFF CACHE INTERNAL "")
set(SK_BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_Declare(
  StereoKitC
  GIT_REPOSITORY https://github.com/StereoKit/StereoKit.git
  GIT_TAG        5efb7404ede3a07c1fe7b14d3914fd60d56fc48d
)
FetchContent_MakeAvailable(StereoKitC)

set(NAK_SRC_CODE
	src/error.h
	src/mf_utility.h
	
	src/nv12_tex.cpp
	src/nv12_tex.h
	src/nv12_sprite.cpp
	src/nv12_sprite.h

	src/mf_encoder.h
	src/mf_encoder.cpp

	src/mf_decoder.h
	src/mf_decoder.cpp
)

set(NAK_EXAMPLES
	src/mf_examples.h

	src/examples/mf_decode_from_url.cpp
	src/examples/mf_roundtrip_webcam.cpp
)

# Add source files to our target executable
add_executable( SKMediaFoundation
  src/main.cpp
  ${NAK_SRC_CODE}
  ${NAK_EXAMPLES}
)

# Link to dependencies
target_link_libraries( SKMediaFoundation
  PRIVATE StereoKitC
)

# Prevent warning C4530
IF(MSVC)
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
ENDIF(MSVC)

# Copy assets to output directory
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  add_custom_command(
	  TARGET SKMediaFoundation PRE_LINK
	  COMMAND ${CMAKE_COMMAND} -E copy_directory
			  ${CMAKE_SOURCE_DIR}/Assets
			  ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/Assets)
endif()

# God's speed if you touch or mess with UWP/WinRT
if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  add_compile_definitions(WINDOWS_UWP=1)
  add_custom_command(
	TARGET SKMediaFoundation PRE_LINK
	COMMAND ${CMAKE_COMMAND} -E copy_directory
			${CMAKE_SOURCE_DIR}/Assets
			${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/Appx/Assets)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT SKMediaFoundation)