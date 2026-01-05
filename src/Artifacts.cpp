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

// Function to generate CMakeLists.txt with IDE source groups and Boost linkage
void generateCMakeFile(const std::string& projectName,
                       const std::string& sourcesDirectory,
                       const std::map<std::string, std::vector<std::string>>& groups,
                       const std::string& readmeContent)
{
    // Determine the parent directory where CMakeLists.txt will be saved
    boost_fs::path sourcesPath(sourcesDirectory);
    boost_fs::path parentPath = sourcesPath.parent_path();
    
    
    // Construct the path to CMakeLists.txt
    boost_fs::path cmakePath = (parentPath.empty() || parentPath == boost_fs::path(".")) ?
    boost_fs::path("CMakeLists.txt") : parentPath / "CMakeLists.txt";
    
    
    // Construct the path to Readme.md
    boost_fs::path readmePath = (parentPath.empty() || parentPath == boost_fs::path(".")) ?
    boost_fs::path("Readme.md") : parentPath / "Readme.md";
    
    // Check if sourcesDirectory exists and is a directory
    if (!boost_fs::exists(sourcesPath) || !boost_fs::is_directory(sourcesPath)) {
        std::cerr << "Error: The sources directory \"" << sourcesDirectory << "\" does not exist or is not a directory." << std::endl;
        return;
    }
    
    // Write the Readme.md file
    if (!readmeContent.empty())
    {
        std::ofstream readmeFile(readmePath.string());
        if (!readmeFile.is_open()) {
            std::cerr << "Failed to open " << readmePath.string() << " for writing." << std::endl;
            return;
        }
        readmeFile << readmeContent;
        readmeFile.close();
    }
    
    // Open the CMakeLists.txt file for writing
    std::ofstream cmakeFile(cmakePath.string());
    if (!cmakeFile.is_open()) {
        std::cerr << "Failed to open " << cmakePath.string() << " for writing." << std::endl;
        return;
    }
    
    // Write basic CMake configurations
    cmakeFile << "cmake_minimum_required(VERSION 3.5)\n";
    cmakeFile << "project(" << projectName << " LANGUAGES CXX)\n\n";
    cmakeFile << "set(CMAKE_CXX_STANDARD 17)\n";
    cmakeFile << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
    cmakeFile << "set(CMAKE_CXX_EXTENSIONS OFF)\n\n";
    
    // Collect all source and header files
    std::vector<boost_fs::path> cppFiles;
    std::vector<boost_fs::path> headerFiles;
    
    for (const auto& entry : boost_fs::directory_iterator(sourcesPath)) {
        if (boost_fs::is_regular_file(entry.status())) {
            boost_fs::path filePath = entry.path();
            
            std::string extension = filePath.extension().string();
            boost_alg::to_lower(extension);
            
            filePath = boost_fs::relative(filePath, sourcesPath);
            
            if (extension == ".cpp") {
                cppFiles.push_back(filePath);
            }
            else if (extension == ".h" || extension == ".hpp") {
                headerFiles.push_back(filePath);
            }
            // Add more extensions if needed
        }
    }
    
    if (cppFiles.empty()) {
        std::cerr << "Error: No source files (.cpp) found in the directory: " << sourcesDirectory << std::endl;
        cmakeFile.close();
        return;
    }
    
    // Set to store all grouped file names for easy lookup
    std::vector<std::string> groupedFiles;
    for (const auto& group : groups) {
        for (const auto& file : group.second) {
            boost_fs::path cppFile = boost_fs::path(sourcesDirectory + "/" + file + ".cpp");
            boost_fs::path headerFile = boost_fs::path(sourcesDirectory + "/" + file + ".h");
            
            if(boost_fs::exists(cppFile))
            {
                std::string cppRelativeFile = file + ".cpp";
                groupedFiles.push_back(cppRelativeFile.c_str());
            }
            if(boost_fs::exists(headerFile))
            {
                std::string headerRelativeFile = file + ".h";
                groupedFiles.push_back(headerRelativeFile.c_str());
            }
        }
    }
    
    // Determine ungrouped files
    std::vector<boost_fs::path> ungroupedCppFiles;
    std::vector<boost_fs::path> ungroupedHeaderFiles;
    
    for (const auto& file : cppFiles) {
        if (std::find(groupedFiles.begin(), groupedFiles.end(), file) == groupedFiles.end()) {
            ungroupedCppFiles.push_back(file);
        }
    }
    
    for (const auto& file : headerFiles) {
        if (std::find(groupedFiles.begin(), groupedFiles.end(), file) == groupedFiles.end()) {
            ungroupedHeaderFiles.push_back(file);
        }
    }
    
    // Write the executable target
    cmakeFile << "add_executable(" << projectName << "\n";
    
    for (const auto& group : groups) {
        for (const auto& file : group.second) {
            boost_fs::path cppFile = boost_fs::path(sourcesDirectory + "/" + file + ".cpp");
            if(boost_fs::exists(cppFile))
            {
                std::string cppRelativeFile = file + ".cpp";
                cmakeFile << "    \"${PROJECT_SOURCE_DIR}/sources/" << cppRelativeFile << "\"\n";
            }
        }
    }
    
    // Add ungrouped .cpp files
    for (const auto& file : ungroupedCppFiles) {
        cmakeFile << "    \"${PROJECT_SOURCE_DIR}/sources/" << file.c_str() << "\"\n";
    }
    
    for (const auto& group : groups) {
        for (const auto& file : group.second) {
            boost_fs::path headerFile = boost_fs::path(sourcesDirectory + "/" + file + ".h");
            if(boost_fs::exists(headerFile))
            {
                std::string headerRelativeFile = file + ".h";
                cmakeFile << "    \"${PROJECT_SOURCE_DIR}/sources/" << headerRelativeFile << "\"\n";
            }
        }
    }
    
    // Ungrouped headers
    for (const auto& file : ungroupedHeaderFiles) {
        cmakeFile << "    \"${PROJECT_SOURCE_DIR}/sources/" << file.c_str() << "\"\n";
    }
    
    cmakeFile << ")\n\n";
    
    // Set include directories if headers are used
    cmakeFile << "target_include_directories(" << projectName << " PRIVATE \"${PROJECT_SOURCE_DIR}/sources/\")\n\n";
    
    for (const auto& group : groups) {
        
        const std::string groupName = replaceDisallowedChars(group.first);
        const std::vector<std::string>& files = group.second;
        
        if (files.empty()) {
            continue; // Skip empty groups
        }
        
        cmakeFile << "source_group(\"" << groupName << "\" FILES\n";
        for (const auto& file : files) {
            boost_fs::path cppFile = boost_fs::path(sourcesDirectory + "/" + file + ".cpp");
            boost_fs::path headerFile = boost_fs::path(sourcesDirectory + "/" + file + ".h");
            if(boost_fs::exists(cppFile) && boost_fs::exists(headerFile))
            {
                std::string cppRelativeFile = file + ".cpp";
                std::string headerRelativeFile = file + ".h";
                
                cmakeFile << "    \"${PROJECT_SOURCE_DIR}/sources/" << headerRelativeFile << "\"\n";
                cmakeFile << "    \"${PROJECT_SOURCE_DIR}/sources/" << cppRelativeFile << "\"\n";
            }
        }
        cmakeFile << ")\n\n";
    }
    
    cmakeFile << "target_compile_definitions(" << projectName << " PRIVATE $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:COMPILE_TEST>)\n\n";
    
    // Assign the README file to a source group if provided
    if (!readmePath.empty()) {
        
        cmakeFile << "add_custom_target(Documentation\n";
        cmakeFile << "    SOURCES \"${PROJECT_SOURCE_DIR}/Readme.md\"\n";
        cmakeFile << ")\n\n";
    }
    
    // Inform about ungrouped files
    if (!ungroupedCppFiles.empty() || !ungroupedHeaderFiles.empty()) {
        std::cout << "Note: Some files are not assigned to any group and will appear directly in the IDE project.\n";
    }
    
    // Close the CMakeLists.txt file
    cmakeFile.close();
    std::cout << "CMakeLists.txt has been generated successfully at \""
              << cmakePath.string() << "\" for project \"" << projectName << "\".\n";
}

}
