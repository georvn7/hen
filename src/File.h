#pragma once

#include <istream>

#include "Reflection.h"

namespace hen {

	class FileName : public Reflection<FileName>
	{
		static std::string			m_promptDir;
		mutable std::string			m_content;
		mutable bool				m_cached;
	public:
		FileName() :m_cached(false) {}
		DECLARE_TYPE(FileName, "describes ASCII encoded text file.")
		DECLARE_FIELD(std::string, type, "The type of the file, could be a simple text file, source code, XML, etc.")
		DECLARE_FIELD(std::string, name, "The name of the file, without the extension.")
		DECLARE_FIELD(std::string, extension, "The extension of the file. A dot and up to 3 letters, for example: .txt, .cpp, .h, .xml, etc")
        DECLARE_FIELD(std::string, hint, "Hint for this step")

		std::string getPath(const std::string& directory) const;
		std::string getContent(const std::string& directory) const;

		static void setPromptDir(const std::string& directory);
        void stripParameters();

        std::string prompt(const std::map<std::string, std::string>& params = {});
	};

    class FileDesc : public Reflection<FileDesc>
    {
    public:
        DECLARE_TYPE(FileDesc, "describes ASCII encoded text file.")
        DECLARE_FIELD(FileName, file_name, "The type of the file, could be a simple text file, source code, XML, etc.")
        DECLARE_FIELD(std::string, description, "Describes the content of the file. Must assume the file is in ASCII encoded string")
    };

	class File : public Reflection<File>
	{
	public:
		DECLARE_TYPE(File, "describes ASCII encoded text file.")
        DECLARE_FIELD(std::string, file_name, "The type of the file, could be a simple text file, source code, XML, etc.")
		DECLARE_FIELD(std::string, content, "The content of the file in an ASCII encoded string")

		bool storeContent(const std::string& directory);
	};

}
