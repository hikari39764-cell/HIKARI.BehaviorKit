cmake_minimum_required(VERSION 3.28)

# 行動樹モジュール（BehaviorKit）。引擎のリポジトリの外で、インストール済みの HIKARI SDK だけを使って編訳する。
# 構築すると <HIKARI_MODULE_PACKAGE_ROOT>/HIKARI.BehaviorKit に挿件包ができる（既定は Library/Plugins）。
# 包の Public/ は、自分の節を登記するゲーム項目や他のモジュールが読む公開ヘッダー。
project(HIKARI_BehaviorKit LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HIKARI modules require x64 Visual Studio 2022.")
endif()

find_package(HIKARI 4.21.0 CONFIG REQUIRED)

# 行動樹の核：樹の資産の形、節の種類の表、編訳、実行器。引擎の関数を呼ばない（行動テストが DLL 無しで使う）。
file(GLOB_RECURSE behaviorkit_core_sources CONFIGURE_DEPENDS LIST_DIRECTORIES false
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/Core/*.h"
     "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.h")
add_library(behaviorkit_core STATIC ${behaviorkit_core_sources})
target_include_directories(
    behaviorkit_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Core" "${CMAKE_CURRENT_SOURCE_DIR}/Public")
target_link_libraries(
    behaviorkit_core
    PUBLIC
        HIKARI::GameProjectSdk
    PRIVATE
        HIKARI::CoreShared)
hikari_configure_module_target(behaviorkit_core)
set_target_properties(behaviorkit_core PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# モジュール本体。Runtime：部品、引擎に触れる節、節を登記する窓口。Editor：樹の編集器（作業区と智能体の操作）。
# 編集器は節の種類の表と、Play 中に走っている樹を直接読むので、同じ DLL に入れる（SINGLE_LIBRARY）。
# Shipping の DLL には Editor が入らない：Shipping は Shipping だけの構築木（game-shipping）で編訳する。
hikari_add_module(
    MANIFEST "HIKARI.BehaviorKit.hmodule"
    SOURCE_DIR Runtime
    EDITOR_SOURCE_DIR Editor
    SINGLE_LIBRARY)
# HIKARI::CoreShared は SDK が配る共用の核（JSON の読み書き）。
target_link_libraries(HIKARI_BehaviorKit PRIVATE behaviorkit_core HIKARI::CoreShared)

# 行動テスト（既定の構築には入らない）：cmake --build --preset editor-release --target HIKARI_BehaviorTreeContracts
# の後、ctest --preset editor-release で走る。
enable_testing()
# 編集器が樹を書き換える部分（TreeModel）は核だけに依るので、ここで一緒に確かめる。
add_executable(HIKARI_BehaviorTreeContracts EXCLUDE_FROM_ALL
               "${CMAKE_CURRENT_SOURCE_DIR}/Tests/HIKARI_BehaviorTreeContracts.cpp"
               "${CMAKE_CURRENT_SOURCE_DIR}/Editor/HIKARI_BehaviorEditorModel.cpp")
target_include_directories(HIKARI_BehaviorTreeContracts PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Editor")
target_link_libraries(HIKARI_BehaviorTreeContracts PRIVATE behaviorkit_core)
hikari_configure_module_target(HIKARI_BehaviorTreeContracts)
foreach(configuration IN ITEMS DEBUG EDITORRELEASE SHIPPING)
    set_target_properties(
        HIKARI_BehaviorTreeContracts PROPERTIES "RUNTIME_OUTPUT_DIRECTORY_${configuration}" "${CMAKE_BINARY_DIR}/tests")
endforeach()
add_test(NAME HIKARI_BehaviorTreeContracts COMMAND "$<TARGET_FILE:HIKARI_BehaviorTreeContracts>")
set_tests_properties(HIKARI_BehaviorTreeContracts PROPERTIES TIMEOUT 60)
