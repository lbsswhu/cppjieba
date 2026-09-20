CMAKE_MINIMUM_REQUIRED (VERSION 3.10)
PROJECT(CPPJIEBA)

INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR}/include)

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 11)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# Define a variable to check if this is the top-level project
if(NOT DEFINED CPPJIEBA_TOP_LEVEL_PROJECT)
    if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
        set(CPPJIEBA_TOP_LEVEL_PROJECT ON)
    else()
        set(CPPJIEBA_TOP_LEVEL_PROJECT OFF)
    endif()
endif()

if(NOT TARGET cppjieba)
  add_library(cppjieba STATIC src/DatBuilder.cpp)
  set_target_properties(cppjieba PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(cppjieba PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_compile_features(cppjieba PUBLIC cxx_std_11)
  if(MSVC)
    target_compile_options(cppjieba PUBLIC /fp:strict /utf-8)
  else()
    target_compile_options(cppjieba PUBLIC -fno-fast-math -ffp-contract=off)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS cppjieba EXPORT cppjiebaTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT cppjiebaTargets DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppjieba)
install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY dict/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/cppjieba/dict)

if(CPPJIEBA_TOP_LEVEL_PROJECT)
  ENABLE_TESTING()

  message(STATUS "MSVC value: ${MSVC}")
  ADD_SUBDIRECTORY(test)
  ADD_TEST(NAME ./test/test.run COMMAND test.run)
  ADD_TEST(NAME ./load_test COMMAND load_test)
endif()
