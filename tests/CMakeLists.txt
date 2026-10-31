cmake_minimum_required(VERSION 3.30)

set(TEST_TARGET_NAME "refactor_tool_tests")

# Рекурсивно ищем все .cpp файлы в папке тестов
file(GLOB TEST_SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Собираем тесты, подключая исходник утилиты из папки src
add_executable(${TEST_TARGET_NAME} ${TEST_SRC_FILES} ${CMAKE_SOURCE_DIR}/src/RefactorTool.cpp)

target_compile_definitions(${TEST_TARGET_NAME} PRIVATE WITHOUT_MAIN)

# Линкуем gtest и стек Clang/LLVM
target_link_libraries(${TEST_TARGET_NAME} 
    PRIVATE 
    gtest
    gtest_main
    clangAST
    clangASTMatchers
    clangFrontend
    clangLex
    clangRewrite
    clangTooling
    clangBasic
    clangSerialization
    clangSema
    clangParse
    clangDriver
    clangEdit
    clangAnalysis
)

# Настраиваем инклюды проекта и Clang
target_include_directories(${TEST_TARGET_NAME} 
    PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_SOURCE_DIR}/src"
    ${CLANG_INCLUDE_DIRS}
    ${LLVM_INCLUDE_DIRS}
)

include(GoogleTest)
gtest_discover_tests(${TEST_TARGET_NAME})
