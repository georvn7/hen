cmake_minimum_required(VERSION 3.25)
project(std-rave VERSION 0.1 DESCRIPTION "std::rave is a long-horizon AI programmer" LANGUAGES CXX C)

# Force use of FindBoost module BEFORE any package calls
cmake_policy(SET CMP0167 OLD)
set(Boost_NO_BOOST_CMAKE ON)
set(BOOST_ROOT "/opt/homebrew")
set(BOOST_LIBRARYDIR "/opt/homebrew/lib")
set(BOOST_INCLUDEDIR "/opt/homebrew/include")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build flags
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DRELEASE")

# Add source files
file(GLOB_RECURSE SRC "src/*.cpp" "src/*.h" "src/*.hpp")
add_executable(${PROJECT_NAME} ${SRC})

# Add this line to list the header explicitly as a source:
target_sources(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Environment/source/black_box_api.h"
)

# And add its folder to the include paths:
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Environment/source
)

# Find Homebrew packages
find_package(cpprestsdk REQUIRED)
find_package(TinyXML2 REQUIRED)

# Find Boost - NO system component (it's header-only now)
find_package(Boost REQUIRED COMPONENTS
    thread program_options filesystem url process)

set(SQLite3_INCLUDE_DIR "/opt/homebrew/opt/sqlite/include")
set(SQLite3_LIBRARY "/opt/homebrew/opt/sqlite/lib/libsqlite3.dylib")
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

# LLVM from Homebrew
set(LLVM_DIR "/opt/homebrew/opt/llvm/lib/cmake/llvm")
find_package(LLVM REQUIRED CONFIG)

find_library(LIB_CLANG clang PATHS ${LLVM_LIBRARY_DIRS})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LLVM_INCLUDE_DIRS}
)

add_definitions(${LLVM_DEFINITIONS})

# Linking - NO Boost::system (it's header-only)
target_link_libraries(${PROJECT_NAME}
    # Homebrew libraries
    Boost::thread
    Boost::program_options
    Boost::filesystem
    Boost::url
    Boost::process
    cpprestsdk::cpprest
    tinyxml2::tinyxml2
    SQLite::SQLite3
    OpenSSL::SSL
    OpenSSL::Crypto
    ${LIB_CLANG}
)

# Platform-Specific Options
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:_DEBUG;_CONSOLE;_WIN32_WINNT=0x0601>
        $<$<CONFIG:Release>:RELEASE;_NDEBUG;_CONSOLE;_WIN32_WINNT=0x0601>
    )
elseif(UNIX AND NOT APPLE)
    # Linux specific options
endif()

# Building for iOS
if(IOS)
    set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO)
    set(CMAKE_IOS_INSTALL_COMBINED YES)
endif()
