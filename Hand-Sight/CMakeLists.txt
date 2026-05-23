# CMakeList.txt : CMake project for Hand-Sight, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.10)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("Hand-Sight")

# Add source to this project's executable.
add_executable (Hand-Sight "Hand-Sight.cpp" "Hand-Sight.h")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET Hand-Sight PROPERTY CXX_STANDARD 20)
endif()

target_include_directories(Hand-Sight PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." )
target_compile_definitions(Hand-Sight PRIVATE UNICODE _UNICODE)
target_link_libraries(Hand-Sight PRIVATE
  ws2_32
  gdi32
  user32
  windowscodecs
  ole32
)

# TODO: Add tests and install targets if needed.
