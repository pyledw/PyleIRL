cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

string(REGEX REPLACE "[^0-9.]" "" _project_version "${_version}")
project(${_name} VERSION ${_project_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

find_path(LIBRIST_INCLUDE_DIR librist/librist.h REQUIRED)
find_library(LIBRIST_LIBRARY librist REQUIRED)
find_library(JANSSON_LIBRARY jansson REQUIRED)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${LIBRIST_INCLUDE_DIR})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${LIBRIST_LIBRARY} ${JANSSON_LIBRARY})

if(ENABLE_QT)
  if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt" "/usr/local/opt/qt")
  endif()
  find_package(Qt6 COMPONENTS Widgets Core Network REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Network)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/srtla-source.c
    src/srtla-stats-filter.c
    src/srtla/srtla_rec.c
    src/srtla/common.c
    src/rist_rec.c
    src/srtla-ui.cpp
    src/srtla-web.cpp
    src/multistream.cpp
    src/resources.qrc
)

if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 crypt32 iphlpapi)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME PyleIRL)

# Fetch and bundle frpc for the Reverse Proxy feature
include(FetchContent)
if(WIN32)
  set(FRPC_URL "https://github.com/fatedier/frp/releases/download/v0.58.1/frp_0.58.1_windows_amd64.zip")
  set(FRPC_BIN "frpc.exe")
elseif(APPLE)
  set(FRPC_URL "https://github.com/fatedier/frp/releases/download/v0.58.1/frp_0.58.1_darwin_amd64.tar.gz")
  set(FRPC_BIN "frpc")
else()
  set(FRPC_URL "https://github.com/fatedier/frp/releases/download/v0.58.1/frp_0.58.1_linux_amd64.tar.gz")
  set(FRPC_BIN "frpc")
endif()

FetchContent_Declare(frp URL ${FRPC_URL})
FetchContent_MakeAvailable(frp)

if(APPLE)
  target_add_resource(${CMAKE_PROJECT_NAME} "${frp_SOURCE_DIR}/${FRPC_BIN}")
else()
  install(PROGRAMS "${frp_SOURCE_DIR}/${FRPC_BIN}" DESTINATION "data/obs-plugins/${CMAKE_PROJECT_NAME}")
endif()
