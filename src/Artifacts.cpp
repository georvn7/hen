#include "Artifacts.h"
#include "IncludeBoost.h"
#include "Utils.h"

namespace stdrave {

DEFINE_TYPE(ProjectArtifact)
DEFINE_ARRAY_FIELD(ProjectArtifact, file_groups)


DEFINE_TYPE(FilesGroup)
DEFINE_FIELD(FilesGroup, group_name)
DEFINE_FIELD(FilesGroup, group_desc)
DEFINE_ARRAY_FIELD(FilesGroup, files)

// Drop-in replacement: generates CMakeLists.txt that
//  - builds main app from src/main/main.cpp (or fallback-detected main)
//  - builds an OBJECT "core" target from all non-test/non-unit_test sources (excluding the app main)
//  - builds one unit-test executable per unitTestDirectories entry (OUTPUT_NAME == "main")
//  - creates runnable Xcode targets run_it_<name> / run_ut_<name> that invoke run.sh and print stdout
//  - optionally registers the same scripts with CTest (ctest -V shows all output)
//
// Expected layout (relative to project root = parent of sourcesDirectory):
//   src/                 -> core sources (and headers)
//   src/main/main.cpp    -> main application entry
//   src/tests/<name>/run.sh
//   src/unit_tests/<name>/main.cpp
//   src/unit_tests/<name>/run.sh
//
// Runner contract:
//   run.sh --exe "<path-to-binary>"
//   (CMake always passes an explicit --exe path; CMake does not parse test.json.)

void generateCMakeFile(const std::string& projectName,
                       const std::string& sourcesDirectory,
                       const std::map<std::string, std::vector<std::string>>& groups,
                       const std::string& readmeContent,
                       const std::vector<std::string>& testDirectories,
                       const std::vector<std::string>& unitTestDirectories)
{
    auto cmakeEscape = [](std::string s) -> std::string {
        // CMake likes forward slashes; also escape quotes.
        for (char& c : s) if (c == '\\') c = '/';
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else out += c;
        }
        return out;
    };

    auto relToProject = [&](const boost_fs::path& projRoot, const boost_fs::path& p) -> boost_fs::path {
        // Try to make a path relative to project root; fall back to absolute.
        try {
            boost_fs::path absP = boost_fs::absolute(p);
            boost_fs::path absR = boost_fs::absolute(projRoot);
            boost_fs::path rel = absP.lexically_relative(absR);
            if (!rel.empty() && rel.native()[0] != '.') return rel;
            // If it's already inside root but starts with ".." etc, just return abs.
        } catch (...) {}
        return boost_fs::absolute(p);
    };

    auto isIgnoredDirName = [](const std::string& name) -> bool {
        // We don't want core sources from these folders under src/
        return (name == "tests" || name == "unit_tests");
    };

    auto fileHasMain = [](const boost_fs::path& p) -> bool {
        // Best-effort detection, used only as fallback if src/main/main.cpp not found.
        // Looks for "int main" substring in the first ~128KB.
        std::ifstream in(p.string(), std::ios::binary);
        if (!in) return false;
        std::string buf;
        buf.resize(128 * 1024);
        in.read(&buf[0], (std::streamsize)buf.size());
        buf.resize((size_t)in.gcount());
        return (buf.find("int main") != std::string::npos);
    };

    boost_fs::path srcPath(sourcesDirectory);
    boost_fs::path projectRoot = srcPath.parent_path();

    if (!boost_fs::exists(srcPath) || !boost_fs::is_directory(srcPath)) {
        std::cerr << "Error: sourcesDirectory does not exist or is not a directory: " << sourcesDirectory << "\n";
        return;
    }
    if (projectRoot.empty() || projectRoot == boost_fs::path(".")) projectRoot = boost_fs::current_path();

    // Where to write:
    const boost_fs::path cmakePath = projectRoot / "CMakeLists.txt";
    const boost_fs::path readmePath = projectRoot / "Readme.md";

    // Write Readme.md (optional)
    if (!readmeContent.empty()) {
        std::ofstream readmeFile(readmePath.string());
        if (!readmeFile.is_open()) {
            std::cerr << "Failed to open " << readmePath.string() << " for writing.\n";
            return;
        }
        readmeFile << readmeContent;
    }

    std::ofstream cmakeFile(cmakePath.string());
    if (!cmakeFile.is_open()) {
        std::cerr << "Failed to open " << cmakePath.string() << " for writing.\n";
        return;
    }

    // Determine src dir name (e.g. "src" or "sources"), used for ${PROJECT_SOURCE_DIR}/<name>
    const std::string srcDirName = srcPath.filename().string();
    const boost_fs::path expectedMain = srcPath / "main" / "main.cpp";

    boost_fs::path appMain = expectedMain;
    if (!boost_fs::exists(appMain)) {
        // Fallbacks
        if (boost_fs::exists(srcPath / "main.cpp")) appMain = srcPath / "main.cpp";
        else {
            // Best-effort: find a .cpp that contains "int main"
            boost_fs::path found;
            for (boost_fs::recursive_directory_iterator it(srcPath), end; it != end; ++it) {
                if (!boost_fs::is_regular_file(it->status())) continue;
                const boost_fs::path p = it->path();
                const std::string ext = boost_alg::to_lower_copy(p.extension().string());
                if (ext != ".cpp" && ext != ".cc" && ext != ".cxx") continue;

                // Skip tests/unit_tests trees
                const auto parentName = p.parent_path().filename().string();
                if (isIgnoredDirName(parentName)) continue;
                if (p.string().find((srcPath / "tests").string()) == 0) continue;
                if (p.string().find((srcPath / "unit_tests").string()) == 0) continue;

                if (fileHasMain(p)) { found = p; break; }
            }
            if (!found.empty()) appMain = found;
        }
    }

    if (!boost_fs::exists(appMain)) {
        std::cerr << "Error: Could not locate application main.cpp. Expected: "
                  << expectedMain.string() << "\n";
        cmakeFile.close();
        return;
    }

    // Collect core sources/headers (recursively), excluding:
    //   - appMain itself
    //   - src/tests/**
    //   - src/unit_tests/**
    std::vector<boost_fs::path> coreSources;
    std::vector<boost_fs::path> headers;

    const boost_fs::path testsRoot = srcPath / "tests";
    const boost_fs::path unitTestsRoot = srcPath / "unit_tests";

    for (boost_fs::recursive_directory_iterator it(srcPath), end; it != end; ++it) {
        const boost_fs::path p = it->path();
        if (boost_fs::is_directory(it->status())) {
            const std::string dn = p.filename().string();
            if (isIgnoredDirName(dn)) {
                it.disable_recursion_pending();
                continue;
            }
            continue;
        }
        if (!boost_fs::is_regular_file(it->status())) continue;

        // Skip anything under tests/unit_tests
        const std::string ps = boost_fs::absolute(p).string();
        if (boost_fs::exists(testsRoot) && ps.find(boost_fs::absolute(testsRoot).string()) == 0) continue;
        if (boost_fs::exists(unitTestsRoot) && ps.find(boost_fs::absolute(unitTestsRoot).string()) == 0) continue;

        // Skip the app main source from "core"
        if (boost_fs::equivalent(p, appMain)) continue;

        std::string ext = p.extension().string();
        boost_alg::to_lower(ext);

        // Paths in CMake should be relative to project root for portability.
        boost_fs::path rel = relToProject(projectRoot, p);

        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
            coreSources.push_back(rel);
        } else if (ext == ".h" || ext == ".hpp" || ext == ".hh") {
            headers.push_back(rel);
        }
    }

    if (coreSources.empty()) {
        std::cerr << "Error: No core source files found under: " << srcPath.string()
                  << " (excluding tests/unit_tests and main)\n";
        cmakeFile.close();
        return;
    }

    // App main path relative to project root
    const boost_fs::path appMainRel = relToProject(projectRoot, appMain);

    // -----------------------------
    // Emit CMake
    // -----------------------------
    cmakeFile << "cmake_minimum_required(VERSION 3.5)\n";
    cmakeFile << "project(" << projectName << " LANGUAGES CXX)\n\n";
    cmakeFile << "set(CMAKE_CXX_STANDARD 17)\n";
    cmakeFile << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
    cmakeFile << "set(CMAKE_CXX_EXTENSIONS OFF)\n\n";

    cmakeFile << "include(CTest)\n";
    cmakeFile << "enable_testing()\n\n";

    cmakeFile << "# Source root (e.g. ${PROJECT_SOURCE_DIR}/" << cmakeEscape(srcDirName) << ")\n";
    cmakeFile << "set(AP_SRC_DIR \"${PROJECT_SOURCE_DIR}/" << cmakeEscape(srcDirName) << "\")\n\n";

    // Lists
    cmakeFile << "set(AP_CORE_SOURCES\n";
    for (const auto& p : coreSources) {
        cmakeFile << "  \"${PROJECT_SOURCE_DIR}/" << cmakeEscape(p.generic_string()) << "\"\n";
    }
    cmakeFile << ")\n\n";

    cmakeFile << "set(AP_HEADERS\n";
    for (const auto& p : headers) {
        cmakeFile << "  \"${PROJECT_SOURCE_DIR}/" << cmakeEscape(p.generic_string()) << "\"\n";
    }
    cmakeFile << ")\n\n";

    cmakeFile << "set(AP_APP_MAIN\n";
    cmakeFile << "  \"${PROJECT_SOURCE_DIR}/" << cmakeEscape(appMainRel.generic_string()) << "\"\n";
    cmakeFile << ")\n\n";

    // Core as OBJECT library so unit tests can reuse it without duplicating main()
    cmakeFile << "add_library(" << projectName << "_core OBJECT\n";
    cmakeFile << "  ${AP_CORE_SOURCES}\n";
    cmakeFile << "  ${AP_HEADERS}\n";
    cmakeFile << ")\n\n";

    cmakeFile << "target_include_directories(" << projectName << "_core PUBLIC \"${AP_SRC_DIR}\")\n\n";

    // Main app
    cmakeFile << "add_executable(" << projectName << "\n";
    cmakeFile << "  $<TARGET_OBJECTS:" << projectName << "_core>\n";
    cmakeFile << "  ${AP_APP_MAIN}\n";
    cmakeFile << ")\n\n";

    cmakeFile << "target_include_directories(" << projectName << " PRIVATE \"${AP_SRC_DIR}\")\n\n";
    cmakeFile << "target_compile_definitions(" << projectName
              << " PRIVATE $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:COMPILE_TEST>)\n\n";

    // Keep your source_group feature for your provided "groups" map (best-effort).
    // NOTE: groups entries are interpreted as paths relative to sourcesDirectory WITHOUT extension (as in your original code).
    for (const auto& group : groups) {
        const std::string groupName = replaceDisallowedChars(group.first);
        const auto& files = group.second;
        if (files.empty()) continue;

        cmakeFile << "source_group(\"" << cmakeEscape(groupName) << "\" FILES\n";
        for (const auto& f : files) {
            // f is something like "Foo" or "sub/Foo" without extension
            boost_fs::path cpp = boost_fs::path(srcDirName) / (f + ".cpp");
            boost_fs::path h   = boost_fs::path(srcDirName) / (f + ".h");
            boost_fs::path hpp = boost_fs::path(srcDirName) / (f + ".hpp");

            // Only emit if exists on disk
            const boost_fs::path absCpp = projectRoot / cpp;
            const boost_fs::path absH   = projectRoot / h;
            const boost_fs::path absHpp = projectRoot / hpp;

            if (boost_fs::exists(absH))   cmakeFile << "  \"${PROJECT_SOURCE_DIR}/" << cmakeEscape(h.generic_string()) << "\"\n";
            if (boost_fs::exists(absHpp)) cmakeFile << "  \"${PROJECT_SOURCE_DIR}/" << cmakeEscape(hpp.generic_string()) << "\"\n";
            if (boost_fs::exists(absCpp)) cmakeFile << "  \"${PROJECT_SOURCE_DIR}/" << cmakeEscape(cpp.generic_string()) << "\"\n";
        }
        cmakeFile << ")\n\n";
    }

    // Documentation target
    if (!readmeContent.empty()) {
        cmakeFile << "add_custom_target(Documentation\n";
        cmakeFile << "  SOURCES \"${PROJECT_SOURCE_DIR}/Readme.md\"\n";
        cmakeFile << ")\n\n";
    }

    // -----------------------------
    // Tests: script-driven (transparent about test.json)
    // Each test dir contains run.sh; CMake passes --exe "<path>" and prints runner output.
    // -----------------------------

    // Emit directories lists (as absolute-or-${PROJECT_SOURCE_DIR}/relative)
    cmakeFile << "set(TEST_DIRECTORIES\n";
    for (const auto& d : testDirectories) {
        boost_fs::path dp(d);
        boost_fs::path rel = relToProject(projectRoot, dp);
        cmakeFile << "  \"${PROJECT_SOURCE_DIR}/" << cmakeEscape(rel.generic_string()) << "\"\n";
    }
    cmakeFile << ")\n\n";

    cmakeFile << "set(UNIT_TEST_DIRECTORIES\n";
    for (const auto& d : unitTestDirectories) {
        boost_fs::path dp(d);
        boost_fs::path rel = relToProject(projectRoot, dp);
        cmakeFile << "  \"${PROJECT_SOURCE_DIR}/" << cmakeEscape(rel.generic_string()) << "\"\n";
    }
    cmakeFile << ")\n\n";

    // Helper functions/macros in CMake
    cmakeFile << R"cmake(
function(ap_add_run_target TARGET_NAME WORKDIR SCRIPT EXE_PATH)
  add_custom_target(${TARGET_NAME}
    COMMAND ${CMAKE_COMMAND} -E chdir "${WORKDIR}"
            /bin/bash "${SCRIPT}" --exe "${EXE_PATH}"
    VERBATIM
  )
endfunction()

function(ap_add_ctest TEST_NAME WORKDIR SCRIPT EXE_PATH)
  add_test(
    NAME ${TEST_NAME}
    COMMAND ${CMAKE_COMMAND} -E chdir "${WORKDIR}"
            /bin/bash "${SCRIPT}" --exe "${EXE_PATH}"
  )
endfunction()

add_custom_target(run_all_tests)
add_custom_target(run_all_unit_tests)
)cmake" << "\n";

    // Integration tests: run.sh uses main app
    cmakeFile << R"cmake(
foreach(dir IN LISTS TEST_DIRECTORIES)
  get_filename_component(name "${dir}" NAME)
  string(REGEX REPLACE "[^A-Za-z0-9_]" "_" name_s "${name}")
  set(script "${dir}/run.sh")

  ap_add_run_target("run_it_${name_s}" "${dir}" "${script}" "$<TARGET_FILE:)cmake"
              << projectName << R"cmake(>" )
  add_dependencies(run_it_${name_s} )cmake" << projectName << R"cmake()

  # Optional: also register with CTest (use `ctest -V` to always see output)
  ap_add_ctest("it_${name_s}" "${dir}" "${script}" "$<TARGET_FILE:)cmake"
              << projectName << R"cmake(>" )

  add_dependencies(run_all_tests run_it_${name_s})
endforeach()

)cmake";

    // Unit tests: build per directory main.cpp, output binary name "main", run script with explicit --exe
    cmakeFile << R"cmake(
foreach(dir IN LISTS UNIT_TEST_DIRECTORIES)
  get_filename_component(name "${dir}" NAME)
  string(REGEX REPLACE "[^A-Za-z0-9_]" "_" name_s "${name}")
  set(script "${dir}/run.sh")

  # Each unit test dir must contain main.cpp (test driver)
  add_executable(ut_${name_s}
    $<TARGET_OBJECTS:)cmake" << projectName << R"cmake(_core>
    "${dir}/main.cpp"
  )

  # The produced executable name should be 'main' (per your convention)
  set_target_properties(ut_${name_s} PROPERTIES OUTPUT_NAME "main")

  target_include_directories(ut_${name_s} PRIVATE "${AP_SRC_DIR}")
  target_compile_definitions(ut_${name_s} PRIVATE $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:COMPILE_TEST>)

  ap_add_run_target("run_ut_${name_s}" "${dir}" "${script}" "$<TARGET_FILE:ut_${name_s}>")
  add_dependencies(run_ut_${name_s} ut_${name_s})

  # Optional: also register with CTest
  ap_add_ctest("ut_${name_s}" "${dir}" "${script}" "$<TARGET_FILE:ut_${name_s}>")

  add_dependencies(run_all_unit_tests run_ut_${name_s})
endforeach()

# A convenience target to run everything with output.
# (In Xcode: build 'run_all_tests' / 'run_all_unit_tests' or 'check')
add_custom_target(check
  COMMAND ${CMAKE_CTEST_COMMAND} -V --output-on-failure
)

)cmake";

    cmakeFile.close();

    std::cout << "CMakeLists.txt generated at \"" << cmakePath.string()
              << "\" for project \"" << projectName << "\".\n";
}


}
