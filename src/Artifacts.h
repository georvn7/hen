#pragma once

#include "Reflection.h"

namespace hen {

void generateCMakeFile(const std::string& projectName,
                       const std::string& sourcesDirectory,
                       const std::map<std::string, std::vector<std::string>>& groups,
                       const std::string& readmeContent,
                       const std::vector<std::string>& testDirectories,
                       const std::vector<std::string>& unitTestDirectories);

class FilesGroup : public Reflection<FilesGroup>
{
public:
    DECLARE_TYPE(FilesGroup, "Group source files or functions by category. It could be by software components or layers")
    DECLARE_FIELD(std::string, group_name, "Name of the group")
    DECLARE_FIELD(std::string, group_desc, "Functional description of the group")
    DECLARE_ARRAY_FIELD(std::string, files, "Files in this group")
};

class ProjectArtifact : public Reflection<ProjectArtifact>
{
public:
    DECLARE_TYPE(ProjectArtifact, "Contins information about the project")
    DECLARE_ARRAY_FIELD(FilesGroup, file_groups, "Functions grouped to serve as filters or virtual folders "\
                                             "in the IDE project files (such as Visual Studio or Xcode). "\
                                             "This will be achieved using CMake's source_group command")
};

}
