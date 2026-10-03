cmake_minimum_required(VERSION 3.15)
project(PhantomArcadeManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(PhantomArcadeManager WIN32
    PhantomArcadeManager.cpp
)

target_link_libraries(PhantomArcadeManager PRIVATE
    ws2_32
    comctl32
    comdlg32
    shell32
    ole32
    user32
    gdi32
    advapi32
)

if (MINGW OR CMAKE_COMPILER_IS_GNUCXX)
    target_link_options(PhantomArcadeManager PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Enable Unicode
target_compile_definitions(PhantomArcadeManager PRIVATE
    UNICODE
    _UNICODE
)
