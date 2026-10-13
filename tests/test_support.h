// 测试专用路径：数据文件与工作目录无关，临时数据库只能建在当前构建目录内。
#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if !defined(LEXICA_TEST_DATA_DIR) || !defined(LEXICA_TEST_OUTPUT_DIR)
#error Build the tests with CMake so their data and output paths are configured.
#endif

namespace test_support {
namespace fs = std::filesystem;

// CMake 提供绝对路径，测试从根目录、构建目录或其他目录启动都能找到词库。
inline fs::path dataFile(const std::string& name) {
    return fs::u8path(LEXICA_TEST_DATA_DIR) / fs::u8path(name);
}

inline fs::path outputRoot() {
    return fs::weakly_canonical(fs::u8path(LEXICA_TEST_OUTPUT_DIR));
}

// 删除前检查真实路径在测试输出目录内部；禁止清理输出根目录或外部目录。
inline void removeDirectory(const fs::path& path) {
    const fs::path root = outputRoot();
    const fs::path target = fs::weakly_canonical(path);
    auto r = root.begin();
    auto t = target.begin();
    for (; r != root.end() && t != target.end() && *r == *t; ++r, ++t) {}
    if (r != root.end() || t == target.end()) {
        throw std::runtime_error("Refusing to remove a directory outside the test output area");
    }
    fs::remove_all(target);
}

// 每个测试进程使用独立子目录，三个测试和多次并行运行不会覆盖彼此的数据。
inline fs::path freshDirectory(const std::string& suite, const std::string& name) {
    const fs::path dir = outputRoot() / fs::u8path(suite) /
                         std::to_string(GetCurrentProcessId()) / fs::u8path(name);
    removeDirectory(dir);
    fs::create_directories(dir);
    return dir;
}

}
