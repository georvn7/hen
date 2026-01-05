#include <regex>
#include <set>

#include "File.h"
#include "Utils.h"
#include "Client.h"

namespace stdrave {

	DEFINE_TYPE(FileName)
	DEFINE_FIELD(FileName, type)
	DEFINE_FIELD(FileName, name)
	DEFINE_FIELD(FileName, extension)
    DEFINE_FIELD(FileName, hint)

	std::string FileName::m_promptDir;

	DEFINE_TYPE(File)
	DEFINE_FIELD(File, file_name);
	DEFINE_FIELD(File, content);

    DEFINE_TYPE(FileDesc)
    DEFINE_FIELD(FileDesc, file_name);
    DEFINE_FIELD(FileDesc, description);

	std::string FileName::getPath(const std::string& directory) const
	{
		std::string filePath = directory + "/";
		filePath += name;
		filePath += extension;
		return filePath;
	}

	std::string FileName::getContent(const std::string& directory) const
	{
		std::ifstream in(getPath(directory));
		std::ostringstream sstr;
		sstr << in.rdbuf();
		return sstr.str();
	}

	void FileName::setPromptDir(const std::string& directory)
	{
		m_promptDir = directory;
	}

    void FileName::stripParameters()
    {
        // Pattern to match parameter placeholders
        std::regex placeholderPattern("\\[\\[([^\\]=]+)(=([^\\]]+))?\\]\\]");
        // Store the original string for comparison after removal
        std::string originalString = m_content;
        
        // Find all parameters
        std::set<std::string> parameters;
        std::string strParams;
        std::sregex_iterator iter(m_content.begin(), m_content.end(), placeholderPattern);
        std::sregex_iterator end;
        while (iter != end) {
            auto isNew = parameters.insert(iter->str());
            if(isNew.second) {
                strParams += iter->str();
                strParams += "; ";
            }
            ++iter;
        }
        
        // Remove all placeholders
        m_content = std::regex_replace(m_content, placeholderPattern, "");

        // Check if any replacements were made by comparing the modified string to the original
        if (m_content != originalString) {
            std::cerr << "Error: The prompt has following parameters in square brackets [[...]] but the input map is empty!" << std::endl;
            std::cerr << strParams << std::endl;
            std::cerr << "All parameters have been removed." << std::endl;
        }
    }

    std::string FileName::prompt(const std::map<std::string, std::string>& params)
	{
		if (!m_cached)
		{
			m_content = getContent(m_promptDir);
			m_cached = true;
		}
        
        if (params.empty()) 
        {
            stripParameters();
            return m_content;
        }
        
        std::string createdPrompt = buildPrompt(m_content, params);
        
        Client::getInstance().setContextPrompt(name);
        if(!hint.empty())
        {
            std::string createdHint = buildPrompt(hint, params);
            Client::getInstance().setStepHint(createdHint);
        }
        
        return createdPrompt;
	}

	bool File::storeContent(const std::string& directory)
	{
		return true;
	}
}
