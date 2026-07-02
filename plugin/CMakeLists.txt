cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# --- IXWebSocket: 서버로 WSS 업링크 (macOS는 SecureTransport TLS, OpenSSL 불필요) ---
include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5) # CMake 4.x 에서 구 의존성 cmake_minimum_required 호환
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
set(USE_TLS ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v11.4.5
)
FetchContent_MakeAvailable(ixwebsocket)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ixwebsocket)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)

# IXWebSocket 서드파티 헤더를 system include 로 처리 → obs 템플릿의 -Werror(-Wnewline-eof 등)에
# 서드파티 헤더 경고가 걸리지 않게 한다.
get_target_property(_ixws_inc ixwebsocket INTERFACE_INCLUDE_DIRECTORIES)
if(_ixws_inc)
  target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PRIVATE ${_ixws_inc})
endif()

# IXWebSocket(서드파티)에는 obs 템플릿의 CMAKE_COMPILE_WARNING_AS_ERROR(-Werror)를 적용하지 않는다.
# (우리 플러그인 타깃은 그대로 엄격하게 유지) — Xcode 제너레이터에서도 동작하는 CMake 네이티브 속성.
if(TARGET ixwebsocket)
  set_target_properties(
    ixwebsocket
    PROPERTIES
      COMPILE_WARNING_AS_ERROR OFF # 비-Xcode 제너레이터
      XCODE_ATTRIBUTE_GCC_TREAT_WARNINGS_AS_ERRORS NO # Xcode 제너레이터(템플릿 전역 YES를 타깃별로 덮음)
  )
endif()

# Windows: IXWebSocket 은 번들 mbedTLS 를 TLS 백엔드로 빌드한다. mbedcrypto 가 CNG 의
# BCryptGenRandom(bcrypt.dll)을 엔트로피로 참조하므로 bcrypt 를 명시 링크해야
# LNK2001(unresolved external symbol BCryptGenRandom)/LNK1120 을 막는다.
# (macOS 는 SecureTransport 라 불필요.)
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE bcrypt)
endif()

# --- nlohmann/json: 서버 모니터링 상태(JSON) 파싱용 (헤더온리) ---
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
# 서드파티 헤더를 SYSTEM include 로 → 템플릿 -Werror 회피 (IXWebSocket 과 동일)
get_target_property(_njson_inc nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
if(_njson_inc)
  target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PRIVATE ${_njson_inc})
endif()

# macOS SDK 26+ 에서 AGL 프레임워크 제거됨. Qt6::Gui 가 의존하는 FindWrapOpenGL.cmake 는
# find_library(WrapOpenGL_AGL) 실패 시에도 "-framework AGL" 을 하드코딩 폴백으로 링크하므로
# (캐시 변수를 비우는 방식으로는 차단 불가) "ld: framework 'AGL' not found" 로 실패한다.
# FindWrapOpenGL.cmake 는 WrapOpenGL::WrapOpenGL 타깃이 이미 존재하면 즉시 반환하므로,
# find_package(Qt6)/obs-frontend-api 전에 타깃을 직접 만들어 AGL 없이 OpenGL 만 링크한다.
# (플러그인은 AGL 심볼을 쓰지 않아 구 SDK 의 CI 러너에서도 동일하게 안전.)
if(APPLE)
  add_library(WrapOpenGL::WrapOpenGL INTERFACE IMPORTED GLOBAL)
  target_link_libraries(WrapOpenGL::WrapOpenGL INTERFACE "-framework OpenGL")
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
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
  PRIVATE src/plugin-main.cpp src/interpreter-engine.cpp src/interpreter-engine.hpp src/interpreter-dock.cpp
          src/interpreter-dock.hpp)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
