cmake_minimum_required(VERSION 3.10)
project(RarusScreenCapture)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force Unicode character set
add_definitions(-DUNICODE -D_UNICODE)

# Add executable for the screen capture service
add_executable(RarusScreenCapture 
    screen_capture_service.cpp
)

# Add executable for the service installer
add_executable(RarusServiceInstaller
    service_installer.cpp
)

# Set Windows subsystem
set_target_properties(RarusScreenCapture PROPERTIES
    WIN32_EXECUTABLE FALSE  # Console application for debugging
)

set_target_properties(RarusServiceInstaller PROPERTIES
    WIN32_EXECUTABLE FALSE  # Console application
)

# Link required libraries
target_link_libraries(RarusScreenCapture
    d3d11        # Direct3D
    dxgi         # DXGI
    windowscodecs # WIC
)

# Create output directory for binary files
set(BINARY_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin)
file(MAKE_DIRECTORY ${BINARY_OUTPUT_DIR})
file(MAKE_DIRECTORY "C:/temp/captures")

# Set output directory for executables
set_target_properties(RarusScreenCapture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BINARY_OUTPUT_DIR}
)

set_target_properties(RarusServiceInstaller PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BINARY_OUTPUT_DIR}
)

# Copy executables to install directory after build
add_custom_command(TARGET RarusScreenCapture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy 
    $<TARGET_FILE:RarusScreenCapture> 
    ${BINARY_OUTPUT_DIR}/RarusScreenCapture.exe
)

add_custom_command(TARGET RarusServiceInstaller POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy 
    $<TARGET_FILE:RarusServiceInstaller> 
    ${BINARY_OUTPUT_DIR}/RarusServiceInstaller.exe
)

# Output successful build message
add_custom_command(TARGET RarusScreenCapture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo 
    "Build complete. Executables can be found in ${BINARY_OUTPUT_DIR}"
)
