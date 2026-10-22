cmake_minimum_required(VERSION 4.2)
project(mendo LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 単一exeで配布するためにCRTを静的リンク
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- MSVCビルド最適化 ----
if(MSVC)
    # /MP: マルチプロセッサコンパイル（各ターゲット内で並列コンパイル）
    add_compile_options(/MP)

    # /GL: プログラム全体の最適化（コンパイル時にLTCGを有効化）
    # /Gy: 関数レベルリンク（リンカが未使用関数を除去可能にする）
    # /Gw: グローバルデータも COMDAT 化し、/OPT:REF で未使用分を除去させる
    add_compile_options($<$<CONFIG:Release>:/GL> $<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw>)

    # /LTCG: リンク時コード生成
    # /OPT:REF: 未参照の関数/データを除去
    # /OPT:ICF: 同一COMDATの統合
    add_link_options(
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )

    # 静的ライブラリにもリンカが処理できるよう/LTCGが必要
    set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /LTCG")
endif()

# ---- Tracy プロファイラ（オプション） ----
# 開発時のフレーム/ゾーン計測用。Release 配布では OFF が前提。
# ON にすると <tracy/Tracy.hpp> が <windows.h> を引き込むため、
# Tracy の zone/frame マクロは profiler.h 経由でラップして使う。
option(MENDO_USE_TRACY "Build with Tracy profiler client" OFF)

include(FetchContent)
# URL 取得の展開ファイルを展開時刻にする。アーカイブの時刻のままだと
# 版更新時に既存のオブジェクトが再ビルドされず新旧が混在する。
cmake_policy(SET CMP0135 NEW)
if(MENDO_USE_TRACY)
    # Tracy v0.14.1 のソース取得（add_subdirectory 相当）。
    # TRACY_ENABLE はクライアント側計装の有効化スイッチで、Tracy の CMakeLists
    # 内で TracyClient のコンパイル定義として伝播する。
    # TRACY_ON_DEMAND は Tracy GUI が接続するまで計測バッファを溜めない設定。
    # アプリ起動だけでメモリを食わないので開発用に都合がよい。
    set(TRACY_ENABLE    ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    set(TRACY_STATIC    ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        tracy
        URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.1.tar.gz
    )
    FetchContent_MakeAvailable(tracy)
    # CRT 設定をプロジェクト全体と揃える（/MT[d]）。
    if(MSVC AND TARGET TracyClient)
        set_target_properties(TracyClient PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(TracyClient PRIVATE /W0)
    endif()
endif()

# ---- Merman ネイティブ C ABI（Mermaid 図表レンダリング用） ----
# Rust ツールチェーンは使わず、公式配布の Windows x64 向けビルド済み DLL をリンクする。
# 配布物は SVG 出力のみを含むため、ラスタライズは LunaSVG で行う。
if(CMAKE_GENERATOR_PLATFORM)
    set(_mendo_arch "${CMAKE_GENERATOR_PLATFORM}")
else()
    set(_mendo_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_mendo_arch}" _mendo_arch)
if(NOT _mendo_arch MATCHES "^(x64|amd64)$")
    message(FATAL_ERROR
        "mendo: merman prebuilt C ABI is currently vendored for Windows x64 only.")
endif()
unset(_mendo_arch)

set(MERMAN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/merman")
add_library(merman_ffi SHARED IMPORTED)
set_target_properties(merman_ffi PROPERTIES
    IMPORTED_LOCATION "${MERMAN_ROOT}/bin/x64/merman_ffi.dll"
    IMPORTED_IMPLIB "${MERMAN_ROOT}/lib/x64/merman_ffi.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${MERMAN_ROOT}/include")
add_library(merman::ffi ALIAS merman_ffi)

# ---- LunaSVG（ネイティブ SVG ラスタライザ） ----
# Merman の C ABI 配布物は PNG を含まないため、SVG を LunaSVG でラスタライズする。
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    lunasvg
    URL https://github.com/sammycage/lunasvg/archive/refs/tags/v3.5.0.tar.gz
)
FetchContent_MakeAvailable(lunasvg)

# ---- md4c（サードパーティ） ----
# Document テキスト表現は UTF-8 (char) 固定。md4c は MD_CHAR=char で動作する。
add_library(md4c STATIC third_party/md4c/md4c.c)
target_include_directories(md4c PUBLIC third_party/md4c)
if(MSVC)
    target_compile_options(md4c PRIVATE /W0)
endif()

# ---- サブディレクトリ定義 ----
set(MENDO_SRC_DIRS
    src src/app src/window src/core src/render src/layout
    src/ui src/input src/io src/mermaid src/nav src/theme src/util
)

# ---- コアライブラリ（テスト可能なコード、WinMain/ウィンドウなし） ----
add_library(mendo_core STATIC
    # core - ドキュメントモデル・パース
    src/core/document.cpp
    src/core/document_utils.cpp
    src/core/anchor.cpp
    src/core/parser.cpp
    src/core/parser_alerts.cpp
    src/core/html_entities.cpp
    src/core/selection_html.cpp
    src/core/syntax.cpp
    # layout - レイアウト計算
    src/layout/layout_cache.cpp
    src/layout/layout.cpp
    src/layout/layout_computer.cpp
    src/layout/dwrite_measurer.cpp
    src/layout/doc_dwrite_bridge.cpp
    # render - 描画パイプライン
    src/render/command_generator.cpp
    src/render/command_executor.cpp
    # app - クリップボード操作 (CopyCodeBlock / SaveDiagramAsPng / CopyDiagramToClipboard)
    src/app/clipboard_manager.cpp
    # ui - UIコンポーネント
    src/ui/context_menu.cpp
    src/ui/pane_controller.cpp
    src/ui/search_state.cpp
    src/ui/settings_dialog.cpp
    src/ui/titlebar.cpp
    src/ui/tooltip.cpp
    # input - 入力処理
    src/input/hit_test_service.cpp
    # io - ファイルI/O・永続化
    src/io/file_loader.cpp
    src/io/file_dialog_service.cpp
    src/io/file_explorer.cpp
    src/io/file_load_service.cpp
    src/io/async_load_coordinator.cpp
    src/io/config_service.cpp
    src/io/image_loader.cpp
    src/io/file_watcher.cpp
    # mermaid - Mermaid図表サポート
    src/mermaid/mermaid_util.cpp
    src/mermaid/mermaid_file_cache.cpp
    # nav - ナビゲーション
    src/nav/nav.cpp
    # theme - テーマ管理
    src/theme/theme.cpp
    src/theme/theme_service.cpp
    # app - アプリケーション制御
    src/app/app_controller.cpp
    src/app/reducer.cpp
    src/app/startup_plan.cpp
    # util - ユーティリティ
    src/util/file_io.cpp
    src/util/pane_layout.cpp
    src/util/task_scheduler.cpp
)
target_include_directories(mendo_core PUBLIC ${MENDO_SRC_DIRS})
target_compile_definitions(mendo_core PUBLIC
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)
target_link_libraries(mendo_core PUBLIC
    md4c d2d1 dwrite windowscodecs
)
if(MENDO_USE_TRACY)
    # MENDO_USE_TRACY: profiler.h のマクロを Tracy 呼び出しに展開するスイッチ。
    # TRACY_ENABLE   : Tracy ヘッダ側の計測コード生成を有効化（Tracy 自体の規約）。
    target_compile_definitions(mendo_core PUBLIC MENDO_USE_TRACY TRACY_ENABLE)
    target_link_libraries(mendo_core PUBLIC Tracy::TracyClient)
endif()
if(MSVC)
    # /Zc:preprocessor: C++20 準拠 preprocessor (__VA_OPT__ 等)。
    # profiler.h の MENDO_LOGF/STATF/TRACEF が __VA_OPT__ を使い、利用側 TU にも展開されるため
    # PUBLIC で伝播させる (依存先の Tracy/md4c には伝播しない)。
    target_compile_options(mendo_core PUBLIC /W4 /utf-8 /Zc:preprocessor)
endif()

# ---- プリコンパイル済みヘッダー ----
# `<pch.h>` 形式で指定するのは、CMake が生成する wrapper に絶対パスを書かれると
# 日本語パス配下では MSVC `/utf-8` と衝突するため。
target_precompile_headers(mendo_core PRIVATE <pch.h>)

# ---- メイン実行ファイル ----
add_executable(mendo WIN32
    src/main.cpp
    src/window/window.cpp
    src/app/app.cpp
    src/app/app_file.cpp
    src/app/app_mouse.cpp
    src/render/renderer.cpp
    src/render/renderer_resources.cpp
    src/render/renderer_pane.cpp
    src/render/renderer_search.cpp
    src/render/d2d_render_backend.cpp
    src/mermaid/mermaid.cpp
    src/mermaid/merman_abi.cpp
    res/mendo.rc
)
target_link_libraries(mendo PRIVATE mendo_core comctl32 delayimp
    merman_ffi lunasvg::lunasvg
)
# Merman の DLL は exe と同じフォルダへ配置する（インポートライブラリは lib/x64）。
add_custom_command(TARGET mendo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MERMAN_ROOT}/bin/x64/merman_ffi.dll"
        "$<TARGET_FILE_DIR:mendo>/merman_ffi.dll"
    VERBATIM)
# ファイルダイアログでしか使わないので、起動時にロードしない (約 1ms)。
# comctl32 は検索の EDIT とツールチップを初めて作るときまで要らない。
# shell32 はドロップ・外部で開く・設定フォルダの取得の予備でしか使わない。
target_link_options(mendo PRIVATE /DELAYLOAD:comdlg32.dll /DELAYLOAD:comctl32.dll /DELAYLOAD:shell32.dll)
# KnownDLLs に無い DLL を静的インポートすると、ローダーが exe と同じフォルダを先に探すので
# Downloads 等に置かれた偽 DLL を読んでしまう。遅延ロードにして、wWinMain 先頭の
# SetDefaultDllDirectories (System32 限定) の後で解決させる。
target_link_options(mendo PRIVATE
    /DELAYLOAD:merman_ffi.dll /DELAYLOAD:d2d1.dll /DELAYLOAD:dwrite.dll /DELAYLOAD:d3d11.dll /DELAYLOAD:dwmapi.dll
    /DELAYLOAD:uxtheme.dll
    /DEPENDENTLOADFLAG:0x800
)
# 自動生成マニフェストを無効化（RC経由で独自のマニフェストを提供するため）
target_link_options(mendo PRIVATE /MANIFEST:NO)

target_precompile_headers(mendo REUSE_FROM mendo_core)

# ---- アセンブラリスト出力（オプション） ----
# /GL 有効時はリストをリンカが LTCG 後のコードで書き出す。出力先はコンパイル時に
# 埋め込まれるため、mendo_core をリンクする mendo_tests も同じファイルを上書きする。
# mendo の結果だけ欲しいときは --target mendo でビルドする。
option(MENDO_ASM_LISTING "Emit source-annotated assembly listings (/FAs)" OFF)
if(MENDO_ASM_LISTING AND MSVC)
    foreach(_mendo_target mendo_core mendo)
        target_compile_options(${_mendo_target} PRIVATE
            /FAs "/Fa${CMAKE_BINARY_DIR}/asm/$<CONFIG>/")
    endforeach()
    unset(_mendo_target)
endif()

# ---- テスト ----
option(MENDO_BUILD_TESTS "Build tests" ON)
if(MENDO_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.zip
    )
    # GoogleTestがCRT設定を上書きするのを防止
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_subdirectory(tests)
endif()

# ---- ベンチマーク ----
# tests/ のヘルパーとモックを使う。それらは gtest を include するので、テストのビルドが要る。
option(MENDO_BUILD_BENCHMARKS "Build benchmarks" OFF)
if(MENDO_BUILD_BENCHMARKS)
    if(NOT MENDO_BUILD_TESTS)
        message(FATAL_ERROR "mendo: MENDO_BUILD_BENCHMARKS requires MENDO_BUILD_TESTS.")
    endif()
    FetchContent_Declare(
        benchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.10.0.zip
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
    add_subdirectory(bench)
endif()
