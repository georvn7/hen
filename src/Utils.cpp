#include <assert.h>

#if defined(_WIN32)
   #include <windows.h>
#elif defined(__APPLE__)
   #include <mach-o/dyld.h>
   #include <limits.h>
#else
   #include <unistd.h>
   #include <linux/limits.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#endif

#include "Utils.h"

#include <boost/filesystem.hpp>

#if defined(__APPLE__) || defined(__linux__)

#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

extern char **environ;
#define EXEC_ENVIRON environ
#else
#define EXEC_ENVIRON nullptr
#endif

using namespace utility;  // Common utilities like string conversions
using namespace web;      // Common features like URIs
using namespace web::json;  // JSON features

namespace hen {

    std::string formatJson(const std::string& inputJson, const std::string& indentChar)
    {
        std::string formattedJson;
        std::stack<char> stack;
        bool inString = false;
        int indentLevel = 0;

        for (size_t i = 0; i < inputJson.size(); ++i) {
            char currentChar = inputJson[i];

            // Handle string literals
            if (currentChar == '\"' && (i == 0 || inputJson[i - 1] != '\\')) {
                inString = !inString;
            }

            if (inString) {
                formattedJson += currentChar;
                continue;
            }

            switch (currentChar) {
            case '{':
            case '[':
                formattedJson += currentChar;
                formattedJson += "\n" + std::string(++indentLevel * indentChar.size(), indentChar[0]);
                break;
            case '}':
            case ']':
                if (indentLevel > 0) {
                    formattedJson += "\n" + std::string(--indentLevel * indentChar.size(), indentChar[0]);
                }
                formattedJson += currentChar;
                break;
            case ',':
                formattedJson += currentChar;
                formattedJson += "\n" + std::string(indentLevel * indentChar.size(), indentChar[0]);
                break;
            case ':':
                formattedJson += currentChar + std::string(" ");
                break;
            default:
                if (currentChar != ' ' && currentChar != '\n' && currentChar != '\t') { // Ignore whitespace outside of strings
                    formattedJson += currentChar;
                }
                break;
            }
        }

        return formattedJson;
    }

    std::string invalidJson(const web::json::value& jsonObject, const web::json::value& jsonSchema)
    {
        std::string strSchema = utility::conversions::to_utf8string(jsonSchema.serialize());
        std::string formatSchema = formatJson(strSchema, "   ");
        std::string strObject = utility::conversions::to_utf8string(jsonObject.serialize());
        std::string formatObject = formatJson(strObject, "   ");

        std::stringstream sout;
        sout << "Invalid JSON object: " << std::endl;
        sout << formatObject << std::endl << std::endl;
        sout << "Check againts the schema!" << std::endl;
        //sout << formatSchema << std::endl;
        return sout.str();
    }

    utility::string_t type(const web::json::value& json)
    {
        switch(json.type())
        {
            case web::json::value::Number:
                return U("Number");
                break;
            case web::json::value::Boolean:
                return U("Boolean");
                break;
            case web::json::value::String:
                return U("String");
                break;
            case web::json::value::Object:
                return U("Object");
                break;
            case web::json::value::Array:
                return U("Array");
                break;
            case web::json::value::Null:
                return U("Null");
            default:
                return U("Unknown");
                break;
        }
        
        return U("Unknown");
    }

    bool isTheSameType(std::stringstream& sserr, const utility::string_t& fieldName, const utility::string_t& schemaType, const web::json::value& jsonValue)
    {
        if(schemaType == U("object") && !jsonValue.is_object())
        {
            sserr << "Mismatching types for field: " << utility::conversions::to_utf8string(fieldName);
            sserr << " in the schema: object, in the json: " << utility::conversions::to_utf8string(type(jsonValue)) << std::endl;
            return false;
        }
        if(schemaType == U("array") && !jsonValue.is_array())
        {
            sserr << "Mismatching types for field: " << utility::conversions::to_utf8string(fieldName);
            sserr << " in the schema: array, in the json: " << utility::conversions::to_utf8string(type(jsonValue)) << std::endl;
            return false;
        }
        else if(schemaType == U("string") && !jsonValue.is_string())
        {
            sserr << "Mismatching types for field: " << utility::conversions::to_utf8string(fieldName);
            sserr << " in the schema: string, in the json: " << utility::conversions::to_utf8string(type(jsonValue)) << std::endl;
            return false;
        }
        else if(schemaType == U("number") && !jsonValue.is_number())
        {
            sserr << "Mismatching types for field: " << utility::conversions::to_utf8string(fieldName);
            sserr << " in the schema: number, in the json: " << utility::conversions::to_utf8string(type(jsonValue)) << std::endl;
            return false;
        }
        
        return true;
    }

    bool validateJsonObject(const web::json::value& jsonObject, const web::json::value& jsonSchema, std::string& log) {

        std::stringstream sout;
        if (!jsonObject.is_object() || !jsonSchema.is_object()) {
            sout << invalidJson(jsonObject, jsonSchema);
            sout << "Invalid input: returned json and the requested schema both must be JSON objects." << std::endl;
            log = sout.str();
            return false;
        }

        auto properties = jsonSchema.as_object().at(U("properties")).as_object();

        // Iterate over schema properties, assume all are required
        for (const auto& schemaField : properties) {
            auto fieldName = schemaField.first;
            auto fieldSchema = schemaField.second;

            // Check if the field exists in the jsonObject
            if (!jsonObject.has_field(fieldName)) {
                sout << invalidJson(jsonObject, jsonSchema);
                sout << "Missing required field: " << conversions::to_utf8string(fieldName) << std::endl;
                log = sout.str();
                return false;
            }

            auto fieldValue = jsonObject.at(fieldName);

            if (fieldSchema.has_field(U("type"))) {
                
                auto fieldType = fieldSchema.at(U("type")).as_string();
                
                if(!isTheSameType(sout, fieldName, fieldType, fieldValue))
                {
                    log = sout.str();
                    return false;
                }

                if (fieldType == U("string") && fieldValue.is_string() && fieldValue.as_string().empty()) {
                    sout << invalidJson(jsonObject, jsonSchema);
                    sout << "Required string field is empty: " << conversions::to_utf8string(fieldName) << std::endl;
                    log = sout.str();
                    return false;
                }
                else if (fieldType == U("object") && fieldValue.is_object()) {
                    // Recursive validation for nested objects
                    if (!validateJsonObject(fieldValue, fieldSchema, log)) {
                        return false;
                    }
                }
                else if (fieldType == U("array") && fieldValue.is_array()) {
                    auto items = fieldValue.as_array();
                    // Validate each item in the array if a schema is provided for items
                    if (items.size() > 0 && fieldSchema.has_field(U("items"))) {
                        auto itemSchema = fieldSchema.at(U("items"));
                        auto itemType = itemSchema.at(U("type")).as_string();
                        if(!isTheSameType(sout, fieldName, itemType, items[0]))
                        {
                            log = sout.str();
                            return false;
                        }
                        
                        if(itemType == U("object"))
                        {
                            for (const auto& item : items) {
                                if (!validateJsonObject(item, itemSchema, log)) {
                                    return false;
                                }
                            }
                        }
                    }
                }
                // Add more type validations as necessary
            }
            else {
                //invalidJson(jsonObject, jsonSchema);
                sout << "Missing field type from the schema for field: " << conversions::to_utf8string(fieldName) << std::endl;
                log = sout.str();
                return false;
            }
        }

        return true; // Passed all validations
    }

    bool validateJson(const web::json::value& jsonObject, const web::json::value& jsonSchema, std::string& log) {
        bool ret = validateJsonObject(jsonObject, jsonSchema, log);
        if (!ret)
        {
            log += "Received invalid JSON:\n";
            log += invalidJson(jsonObject, jsonSchema);
        }
        return ret;
    }

    std::string getFirstSubdirectory(const boost_fs::path& dirPath) {
        boost_fs::directory_iterator end_iter; // Default-constructed iterator acts as the end iterator.
        for (boost_fs::directory_iterator dir_itr(dirPath); dir_itr != end_iter; ++dir_itr) {
            if (boost_fs::is_directory(dir_itr->status())) { // Check if the entry is a directory.
                // Return the name of the first sub-directory found.
                return dir_itr->path().filename().string();
            }
        }
        return ""; // Return an empty string if no sub-directory is found.
    }

    IncludeType getIncludeType(const std::string& includeStatement) {
        // Trim leading whitespace (not strictly necessary depending on input)
        size_t start = includeStatement.find_first_not_of(" \t");
        if (start == std::string::npos) return IncludeType::Invalid;

        // Check if the string starts correctly with '#include'
        if (includeStatement.substr(start, 8) != "#include") return IncludeType::Invalid;

        // Find start of the include path
        size_t pathStart = includeStatement.find_first_not_of(" \t", start + 8);
        if (pathStart == std::string::npos) return IncludeType::Invalid;

        // Determine the type of include based on the first character after "#include "
        if (includeStatement[pathStart] == '<') {
            // Check if there is a corresponding '>'
            if (includeStatement.find('>', pathStart) != std::string::npos) {
                return IncludeType::AngleBracket;
            }
        }
        else if (includeStatement[pathStart] == '"') {
            // Check if there is a corresponding '"'
            if (includeStatement.find('"', pathStart + 1) != std::string::npos) {
                return IncludeType::DoubleQuote;
            }
        }

        return IncludeType::Invalid;
    }

    std::string getLastToken(const std::string& str, char delimiter) {
        // Find the last occurrence of the delimiter
        size_t pos = str.find_last_of(delimiter);
        if (pos != std::string::npos) {
            // Extract and return the substring after the last delimiter
            return str.substr(pos + 1);
        }
        return str; // Return the whole string if no delimiter is found
    }

    // Function to convert JSON Schema to C++ struct
    std::string jsonSchemaToCStruct(web::json::value& schema) {
        try {
            // Parse the JSON schema string into a JSON object
            //auto schema = json::value::parse(jsonSchemaStr);

            // We expect the schema to describe a simple object with properties
            if (schema[U("type")].as_string() != U("object") || !schema.has_field(U("properties"))) {
                return "Invalid schema: must be an object with properties";
            }

            // Start building the C++ struct
            std::string structStr = "struct MyStruct {\n";
            std::map<std::string, std::string> enumDefs;

            // Iterate over properties
            auto properties = schema[U("properties")].as_object();
            for (auto& pair : properties) {
                auto key = pair.first;
                auto& value = pair.second;
                std::string cType;

                if (value.has_field(U("$ref"))) {
                    // Handle reference to another schema
                    std::string ref = conversions::to_utf8string(value[U("$ref")].as_string());
                    cType = "struct " + ref.substr(ref.find_last_of('/') + 1) + "*";
                }
                else {
                    std::string type = conversions::to_utf8string(value[U("type")].as_string());

                    // Map JSON types to C++ types
                    if (type == "integer") {
                        cType = "int";
                    }
                    else if (type == "number") {
                        cType = "double";
                    }
                    else if (type == "string") {
                        cType = "std::string";
                    }
                    else if (type == "boolean") {
                        cType = "bool";
                    }
                    else if (type == "array") {
                        // Check for the type of the items in the array
                        std::string itemType = conversions::to_utf8string(value[U("items")][U("type")].as_string());
                        if (itemType == "string") {
                            cType = "std::vector<std::string>";
                        }
                        else if (itemType == "integer") {
                            cType = "std::vector<int>";
                        }
                        else if (itemType == "number") {
                            cType = "std::vector<double>";
                        }
                    }
                    else {
                        cType = "void*";  // unknown type
                    }

                    // Check for enum and create a C++ enum type if present
                    if (value.has_field(U("enum"))) {
                        std::string enumName = "Enum_" + conversions::to_utf8string(key);
                        cType = enumName;
                        enumDefs[enumName] = "enum class " + enumName + " {\n";
                        for (const auto& enumVal : value[U("enum")].as_array()) {
                            if (type == "string") {
                                enumDefs[enumName] += "    " + conversions::to_utf8string(enumVal.as_string()) + ",\n";
                            }
                            else if (type == "integer") {
                                enumDefs[enumName] += "    " + enumName + "_" + std::to_string(enumVal.as_integer()) + ",\n";
                            }
                        }
                        enumDefs[enumName].pop_back();  // Remove the last newline
                        enumDefs[enumName].pop_back();  // Remove the last comma
                        enumDefs[enumName] += "\n};\n";
                    }
                }

                // Append the field to the struct
                structStr += "    " + cType + " " + conversions::to_utf8string(key) + ";\n";
            }

            // Close the struct
            structStr += "};\n";

            // Prepend any enum definitions to the struct string
            for (auto& enumDef : enumDefs) {
                structStr = enumDef.second + structStr;
            }

            return structStr;
        }
        catch (const web::json::json_exception& e) {
            // Handle errors (e.g., parsing errors)
            return "Error parsing JSON Schema: " + std::string(e.what());
        }
    }

std::string extractDataType(const std::string& type)
{
    // Regex pattern to handle various cases including char*, const, pointers, references, templates, and arrays
    std::regex typeRegex(R"((?:\bconst\b\s*)*((?:unsigned\s+|signed\s+|long\s+|short\s+|)\b[\w:]+(?:<[^>]*>)?)(?:\s*[*&\s*const]*)*(?:\s*\[\s*\d*\s*\])*)");
    std::smatch match;
    
    // Check if the type contains a template
    if (type.find('<') != std::string::npos && type.find('>') != std::string::npos)
    {
        return "";
    }
    
    if (std::regex_search(type, match, typeRegex))
    {
        return match[1].str();
    }
    
    return "";
}

std::string extractType(const std::string& declaration, const std::string& name)
{
    std::vector<std::string> tokens;
    
    std::string removedComments;
    std::string noComments = removeComments(declaration, removedComments);
    
    // Split the string by all types of whitespace
    boost::split(tokens, noComments, boost::is_space(), boost::token_compress_on);
    std::string type;
    
    for(int i=0; i < tokens.size(); ++i) {
        if(tokens[i] == name) {
            break;
        }
        
        if(!type.empty()) {
            type += " ";
        }
        type += tokens[i];
    }
    
    return type;
}

std::string removeWhitespace(const std::string &str) {
    std::string result = str;
    result.erase(std::remove_if(result.begin(), result.end(), [](unsigned char c) { return std::isspace(c); }), result.end());
    return result;
}


std::string removeComments(const std::string &input, std::string &removed_out) {
    std::ostringstream output;
    std::ostringstream removed;      // collect stripped comments here

    bool in_single_line_comment = false;
    bool in_multi_line_comment  = false;

    bool in_double_quote = false;  // "..."
    bool in_single_quote = false;  // '...'

    bool last_char_was_backslash = false;  // Track escaping inside quotes

    std::size_t comment_start = std::string::npos;

    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        // 1) If currently in a single-line comment, skip until newline
        if (in_single_line_comment) {
            if (c == '\n') {
                // capture the whole comment (without the newline)
                if (comment_start != std::string::npos) {
                    removed << input.substr(comment_start, i - comment_start) << std::endl;
                    comment_start = std::string::npos;
                }
                in_single_line_comment = false;
                output << c;  // keep the newline in the code output
            }
            continue;
        }

        // 2) If currently in a multi-line comment, skip until "*/"
        if (in_multi_line_comment) {
            if (c == '*' && (i + 1) < input.size() && input[i + 1] == '/') {
                // capture the whole comment including the closing "*/"
                if (comment_start != std::string::npos) {
                    removed << input.substr(comment_start, (i + 2) - comment_start) << std::endl;
                    comment_start = std::string::npos;
                }
                in_multi_line_comment = false;
                ++i; // skip the '/'
            }
            continue;
        }

        // 3) Inside string/char literals: only exit on unescaped matching quote.
        if (in_double_quote) {
            if (c == '"' && !last_char_was_backslash) {
                in_double_quote = false;
            }
            output << c;
            last_char_was_backslash = (!last_char_was_backslash && c == '\\');
            continue;
        }
        if (in_single_quote) {
            if (c == '\'' && !last_char_was_backslash) {
                in_single_quote = false;
            }
            output << c;
            last_char_was_backslash = (!last_char_was_backslash && c == '\\');
            continue;
        }

        // 4) Not in a quote or comment: check starts of comments or quotes.

        // Single-line comment starts?
        if (c == '/' && (i + 1) < input.size() && input[i + 1] == '/') {
            in_single_line_comment = true;
            comment_start = i;  // remember where the comment begins
            ++i;                // skip the second '/'
            continue;
        }

        // Multi-line comment starts?
        if (c == '/' && (i + 1) < input.size() && input[i + 1] == '*') {
            in_multi_line_comment = true;
            comment_start = i;  // remember where the comment begins
            ++i;                // skip the '*'
            continue;
        }

        // Double-quote literal starts?
        if (c == '"') {
            in_double_quote = true;
            output << c;
            last_char_was_backslash = false;
            continue;
        }

        // Single-quote literal starts?
        if (c == '\'') {
            in_single_quote = true;
            output << c;
            last_char_was_backslash = false;
            continue;
        }

        // Normal character → output it
        output << c;
        last_char_was_backslash = false;
    }

    // Handle unterminated comments at EOF
    if (in_single_line_comment && comment_start != std::string::npos) {
        removed << input.substr(comment_start) << std::endl;
    }
    if (in_multi_line_comment && comment_start != std::string::npos) {
        removed << input.substr(comment_start) << std::endl;
    }

    removed_out = removed.str();
    return output.str();
}

std::string getSourceType(const std::string& fileExtension)
{
    static std::map<std::string, std::string> s_sourceTypes = {
        {".h", "cpp"},
        {".c", "cpp"},
        {".cpp", "cpp"},
        {".hpp", "cpp"},
        {".txt", "txt"},
        {".json", "json"},
    };
    
    auto it = s_sourceTypes.find(fileExtension);
    if(it == s_sourceTypes.end())
    {
        return "txt";
    }
    
    return s_sourceTypes[fileExtension];
}

bool hasMainFunction(const std::string& objFilePath) {
    namespace bp = boost::process;
    namespace fs = boost::filesystem;

    //Quick hack
    
    // Check if the file exists
    if (!fs::exists(objFilePath) || !fs::is_regular_file(objFilePath)) {
        std::cerr << "File does not exist or is not a regular file: " << objFilePath << std::endl;
        return false;
    }

    // Extract the filename
    fs::path filePath(objFilePath);
    std::string filename = filePath.filename().string();

    // Check if the filename is main.o
    return filename == "main.o";
}

std::string checkStringEnd(const std::string& input) {
    // Define the keywords to check for
    const std::string success = "SUCCESS";
    const std::string failure = "FAILURE";

    // Trim whitespace from both ends of the input string
    auto trimmedEnd = input.find_last_not_of(" \t\n\r\f\v");
    if (trimmedEnd == std::string::npos) {
        return "UNKNOWN";
    }
    auto trimmed = input.substr(0, trimmedEnd + 1);
    
    // Check if trimmed ends with SUCCESS or FAILURE
    if (trimmed.size() >= success.size() &&
        trimmed.compare(trimmed.size() - success.size(), success.size(), success) == 0) {
        return "SUCCESS";
    } else if (trimmed.size() >= failure.size() &&
               trimmed.compare(trimmed.size() - failure.size(), failure.size(), failure) == 0) {
        return "FAILURE";
    }

    return "UNKNOWN";
}

static int wait_with_timeout(pid_t pid, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;

    while (true) {
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) return status;         // finished
        if (r == -1) return -1;              // error

        if (std::chrono::steady_clock::now() >= deadline) {
            return -2; // timeout sentinel
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

ExecResult exec_with_timeout(const std::string& cmd,
                             const std::string& workingDir,
                             const std::string& operation,
                             bool deleteOutput,
                             std::chrono::milliseconds timeout)
{
    const std::string workingDirectory = boost_fs::absolute(workingDir).string();
    const std::string outputFilePath   = workingDirectory + "/" + operation + "Output.txt";
    const std::string commandFilePath  = workingDirectory + "/" + operation + "Command.txt";

    // Ensure no stale output
    if (boost_fs::exists(outputFilePath)) {
        boost_fs::remove(outputFilePath);
    }

    {
        std::ofstream header(commandFilePath);
        header << cmd;
    }

    // Build a single shell command that cd's + redirects output
#if defined(__APPLE__) || defined(__MACH__)
    const char* shell = "/bin/zsh";
    const char* shellFlag = "-c";
#else
    const char* shell = "/bin/bash";
    const char* shellFlag = "-c";
#endif

    // Quote paths minimally: safest is to avoid shell, but we're keeping your model.
    std::string shellCmd;
    shellCmd += "cd ";
    shellCmd += "'";
    for (char c : workingDirectory) shellCmd += (c == '\'' ? std::string("'\\''") : std::string(1, c));
    shellCmd += "'";
    shellCmd += " && ( ";
    shellCmd += cmd;
    shellCmd += " ) > ";
    shellCmd += "'";
    for (char c : outputFilePath) shellCmd += (c == '\'' ? std::string("'\\''") : std::string(1, c));
    shellCmd += "'";
    shellCmd += " 2>&1";

    // Prepare argv for shell
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(shell));
    argv.push_back(const_cast<char*>(shellFlag));
    argv.push_back(const_cast<char*>(shellCmd.c_str()));
    argv.push_back(nullptr);

    // Spawn in its own process group so we can kill the whole tree
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

    short flags = 0;
#ifdef POSIX_SPAWN_SETPGROUP
    flags |= POSIX_SPAWN_SETPGROUP;
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setpgroup(&attr, 0); // child pid becomes pgid
#endif

    pid_t pid = -1;
    int spawn_rc = posix_spawn(&pid, shell, nullptr, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);

    if (spawn_rc != 0) {
        throw std::runtime_error("posix_spawn failed: " + std::to_string(spawn_rc));
    }

    // Wait with timeout
    int status = wait_with_timeout(pid, timeout);
    bool timed_out = false;
    int exit_code = -1;

    if (status == -2) {
        timed_out = true;

        // Kill process group: -pid means "process group pid"
        ::kill(-pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::kill(-pid, SIGKILL);

        // Reap
        (void)::waitpid(pid, &status, 0);
    } else if (status >= 0) {
        if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);
    }

    // Read output (if exists)
    std::string result;
    {
        std::ifstream inFile(outputFilePath);
        if (inFile) {
            result.assign((std::istreambuf_iterator<char>(inFile)),
                          std::istreambuf_iterator<char>());
        }
    }

    // Trim trailing newlines
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }

    if (deleteOutput && boost_fs::exists(outputFilePath)) {
        boost_fs::remove(outputFilePath);
    }

    return ExecResult{result, exit_code, timed_out};
}


std::string exec(const std::string& cmd, const std::string& workingDir, const std::string& operation, bool deleteOutput)
{
    // Execute the setup file and store the result
    // This is a simplistic approach; consider using a library for process management
    // Redirecting output to a file and then reading it might be one approach
    std::string workingDirectory = boost_fs::absolute(workingDir).string();
    std::string outputFilePath = workingDirectory + "/" + operation + "Output.txt";
    std::string command = "cd " + workingDirectory + "/; ( ";
    command += cmd + " ) > " + outputFilePath + " 2>&1";
    
    //We don't want to return something from previouse execution
    if(boost_fs::exists(outputFilePath)) {
        boost_fs::remove(outputFilePath);
    }
    
    std::ofstream header(workingDirectory + "/" + operation + "Command.txt");
    header << cmd;
    header.close();
    
#if defined(__APPLE__) || defined(__MACH__)
    //assert(pthread_main_np()!=0);
    int ret = std::system(("zsh -c \"" + std::string(command) + "\"").c_str());
#else
    int ret = std::system(command.c_str());
#endif

    std::string result;
    std::ifstream inFile(outputFilePath);
    if (inFile) {
        result.assign((std::istreambuf_iterator<char>(inFile)),
            std::istreambuf_iterator<char>());
        inFile.close();
        
        // Remove trailing newlines
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
        
        if(deleteOutput)
        {
            boost_fs::remove(outputFilePath);
        }
        return result;
    }
    
    std::stringstream sout;
    sout << "Command: " << cmd << std::endl;
    sout << "Working directory: " << workingDirectory << std::endl;
    sout << "Unable to open the output file: " << outputFilePath << std::endl;
    
    return sout.str();
}

std::vector<std::string> parseCommandLine(const std::string &cmdLine)
{
    std::istringstream iss(cmdLine);
    std::vector<std::string> args;
    std::string token;

    // This reads space-separated tokens, respecting quotes.
    // Example: --option1 "some text" => {"--option1", "some text"}
    while (iss >> std::quoted(token))
    {
        args.push_back(token);
    }
    return args;
}

std::unique_ptr<boost_prc::process> spawn(const std::string& cmd, const std::string& workingDir,
                                         const std::string& outputDir, const std::string& operation,
                                         bool detached)
{
    // Setup paths (same as before)
    std::string timestamp = getCurrentDateTime();
    std::string workingDirectory = boost_fs::absolute(workingDir).string();
    std::string outputDirectory = boost_fs::absolute(outputDir).string();
    std::string outputFilePath = outputDirectory + "/Log-" + timestamp + "-" + operation + ".txt";
    std::string commandFilePath = outputDirectory + "/Cmd-" + timestamp + "-" + operation + ".txt";

    // Save command to file
    std::ofstream header(commandFilePath);
    header << cmd;
    header.close();

    // Parse command line using your function
    std::vector<std::string> tokens = parseCommandLine(cmd);
    
    if (tokens.empty()) {
        throw std::invalid_argument("Empty command");
    }
    
    std::string executable = tokens[0];
    std::vector<std::string> args(tokens.begin() + 1, tokens.end());

    // Create io_context (required for v2)
    static thread_local boost::asio::io_context ctx;

    // Launch process with v2 API
    auto process = std::make_unique<boost_prc::process>(
        ctx,
        executable,
        args,
        boost_prc::process_stdio{
            nullptr,
            boost_fs::path(outputFilePath),
            boost_fs::path(outputFilePath)
        },
        boost_prc::process_start_dir(workingDirectory)
    );

    // Detach if requested
    if (detached) {
        process->detach();
    }
    
    return process;
}

bool isDebuggerAttached() {
#ifdef _WIN32
    return IsDebuggerPresent();
#elif __APPLE__
    int mib[4];
    struct kinfo_proc info;
    size_t size = sizeof(info);

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    sysctl(mib, 4, &info, &size, nullptr, 0);
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#elif __linux__
    return ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == -1;
#endif
}

void waitForDebugger() {
    std::cout << "Waiting for debugger to attach..." << std::endl;
    while (!isDebuggerAttached()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Debugger attached!" << std::endl;
}

std::string getLANIP() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return "";
    }
    
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        WSACleanup();
        return "";
    }
    
    struct hostent* host = gethostbyname(hostname);
    if (host == nullptr) {
        WSACleanup();
        return "";
    }
    
    std::string ip = inet_ntoa(*((struct in_addr*)host->h_addr_list[0]));
    WSACleanup();
    return ip;
#else
    struct ifaddrs* ifAddrStruct = nullptr;
    if (getifaddrs(&ifAddrStruct) != 0) {
        return "";
    }
    
    for (struct ifaddrs* ifa = ifAddrStruct; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
            void* tmpAddrPtr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
            char addressBuffer[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
            
            std::string interface(ifa->ifa_name);
            if (interface != "lo") {
                freeifaddrs(ifAddrStruct);
                return std::string(addressBuffer);
            }
        }
    }
    
    freeifaddrs(ifAddrStruct);
    return "";
#endif
}


web::json::value findInJson(const web::json::value& object, const std::string& propertyPath)
{
    //"calls.json:items[func_name==\"handle_error\"]"
    std::vector<std::string> pathElements;
    boost::split(pathElements, propertyPath, boost::is_any_of("."));
    
    for(const auto& element : pathElements)
    {
        std::vector<std::string> expression;
        boost::split(expression, element, boost::is_any_of("[=]"));
        
        if(object.has_field(U(expression[0])))
        {
            web::json::value property = object.at(U(expression[0]));
            if(property.is_array() && expression.size() > 1)
            {
                if(expression.size() >= 3)
                {
                    auto propertyAsArray = property.as_array();
                    for(int i=0; i<propertyAsArray.size(); ++i)
                    {
                        if(propertyAsArray[i].has_field(U(expression[1])) &&
                           propertyAsArray[i][U(expression[1])].as_string() == U(expression[2]))
                        {
                            
                            return propertyAsArray[i];
                        }
                    }
                }
            }
            else
            {
                return property;
            }
        }
    }
    
    return web::json::value();
}

// Function to check if s1 starts with s2, ignoring leading whitespace in s1 and case sensitivity
bool startsWithIgnoreCase(const std::string &s1, const std::string &s2) {
    std::string trimmedS1 = trimLeadingWhitespace(s1);
    std::string lowerS1 = toLower(trimmedS1);
    std::string lowerS2 = toLower(s2);
    return lowerS1.find(lowerS2) == 0;
}

// Removes: [leading ws] + word + [ws after word]
// Only if `word` is the first token (after leading whitespace).
std::string removeFirstWord(const std::string& from, const std::string& word)
{
    const auto is_ws = [](char c) -> bool {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };

    if (word.empty()) return from;

    const std::size_t n = from.size();

    // 1) Skip leading whitespace
    std::size_t i = 0;
    while (i < n && is_ws(from[i])) ++i;

    // 2) Must match `word` at the first token position
    if (i + word.size() > n) return from;
    if (from.compare(i, word.size(), word) != 0) return from;

    // 3) Ensure token boundary (end or whitespace)
    const std::size_t k = i + word.size();
    if (k < n && !is_ws(from[k])) return from;

    // 4) Skip whitespace after the word
    std::size_t j = k;
    while (j < n && is_ws(from[j])) ++j;

    return from.substr(j);
}


void setupEnv()
{
#if defined(__APPLE__)
    // Avoid mutating PATH implicitly. If a caller needs extra tool locations,
    // they can provide them explicitly via HEN_EXTRA_PATH.
    const char* extraPath = std::getenv("HEN_EXTRA_PATH");
    if(!extraPath || !*extraPath)
    {
        return;
    }
    
    const char* currentPath = std::getenv("PATH");
    std::string newPathValue(extraPath);
    if(currentPath && *currentPath)
    {
        newPathValue += ":";
        newPathValue += currentPath;
    }
    
    if(setenv("PATH", newPathValue.c_str(), 1) != 0)
    {
        std::cerr << "Failed to set PATH environment variable." << std::endl;
        std::exit(EXIT_FAILURE);
    }
#endif
}

std::string getCxxCompilerCommand()
{
    const char* explicitCompiler = std::getenv("HEN_CXX");
    if(explicitCompiler && *explicitCompiler)
    {
        return explicitCompiler;
    }
    
#if defined(__APPLE__)
    return "/usr/bin/xcrun clang++";
#else
    return "clang++";
#endif
}

std::string buildPrompt(const std::string& content, const std::map<std::string, std::string>& params)
{
    std::regex placeholderPattern(R"(\[\[([^\]=]+)(=([^\]]+))?\]\])");
    std::smatch matches;

    std::string resultString = content;

    auto begin = std::sregex_iterator(content.begin(), content.end(), placeholderPattern);
    auto end = std::sregex_iterator();
    for (std::sregex_iterator i = begin; i != end; ++i) {
       matches = *i;
       std::string fullMatch = matches.str(0);
       std::string paramName = matches.str(1);
       std::string defaultValue = matches.length(3) > 0 ? matches.str(3) : "";

       std::string replacementValue;
       auto paramValueIt = params.find(paramName);
       if (paramValueIt != params.end()) {
           replacementValue = paramValueIt->second;
       } else if (!defaultValue.empty()) {
           replacementValue = defaultValue;
       } else {
           std::cerr << "Error: No value provided for parameter \"" << paramName << "\" and no default value specified.\n";
           replacementValue = "";
       }

       std::regex specificPlaceholderPattern(R"(\[\[)" + escapeRegex(paramName) + R"((=[^\]]+)?\]\])");
       resultString = std::regex_replace(resultString, specificPlaceholderPattern, replacementValue);
    }
    
    return resultString;
}

bool addToSet(std::vector<std::shared_ptr<std::string>>& vset, const std::string& element)
{
    if(element.empty())
    {
        std::cout << "Adding empty element" << std::endl;
    }
    
    auto it = std::find_if(vset.begin(),
                           vset.end(),
                           [&element](const std::shared_ptr<std::string>& str_ptr) {
        return *str_ptr == element;
    });
    
    if (it == vset.end()) {
        
        vset.push_back(std::make_shared<std::string>(element));
        return true;
    }
    
    return false;
}

bool isInSet(std::vector<std::shared_ptr<std::string>>& vset, const std::string& element)
{
    auto it = std::find_if(vset.begin(),
                           vset.end(),
                           [&element](const std::shared_ptr<std::string>& str_ptr) {
        return *str_ptr == element;
    });
    
    return it != vset.end();
}

std::string getAsCsv(const std::set<std::string>& namesSet, int max /*= -1*/)
{
    std::string csvList;
    const bool limited = (max >= 0);
    int emitted = 0;
    
    for (const auto& name : namesSet)
    {
        if (limited && emitted >= max)
        {
            break;
        }
        
        if (!csvList.empty())
        {
            csvList += ", ";
        }
        
        csvList += name;
        ++emitted;
    }
    
    if (limited)
    {
        const int remaining = static_cast<int>(namesSet.size()) - emitted;
        if (remaining > 0)
        {
            if (!csvList.empty())
            {
                csvList += ", ";
            }
            csvList += "... (+" + std::to_string(remaining) + " more)";
        }
    }
    
    return csvList;
}

// Helper function to trim whitespace from both ends of a string.
std::string trim(const std::string& str) {
    auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();
    return (start < end) ? std::string(start, end) : "";
}

std::vector<std::shared_ptr<std::string>> readFromCsv(const std::string& csv)
{
    std::vector<std::shared_ptr<std::string>> result;
    std::istringstream stream(csv);
    std::string token;
    
    // Read each token separated by a comma.
    while (std::getline(stream, token, ','))
    {
        // Trim any leading/trailing whitespace.
        token = trim(token);
        // Store each token in a shared_ptr and add to the result.
        result.push_back(std::make_shared<std::string>(token));
    }
    
    return result;
}

std::string getAsList(const std::vector<std::shared_ptr<std::string>>& namesSet)
{
    std::string csvList;
    for(auto name : namesSet)
    {
        if(!csvList.empty())
        {
            csvList += "\n";
        }
        csvList += *name;
    }
    
    return csvList;
}

std::string generateUUID()
{
    // Create a random generator
    static boost::uuids::random_generator gen;

    // Generate a UUID
    boost::uuids::uuid uuid = gen();

    // Convert UUID to string
    return boost::uuids::to_string(uuid);
}

uint32_t generateUniqueUint32()
{
    static boost::uuids::random_generator gen;
    boost::uuids::uuid uuid = gen();
    
    uint32_t result;
    std::memcpy(&result, uuid.data, sizeof(uint32_t));
    return result;
}

std::string getCurrentDateTime()
{
    // Get the current time_point from system_clock
    auto now = std::chrono::system_clock::now();

    // Convert to time_t for use with localtime_r or localtime_s
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);

    // Prepare a tm structure to receive the time components
    std::tm tm_now;

    // Handle platform-specific differences
#if defined(_WIN32) || defined(_WIN64)
    // Windows - use localtime_s
    localtime_s(&tm_now, &now_time_t);
#else
    // POSIX (Linux, macOS, etc.) - use localtime_r
    localtime_r(&now_time_t, &tm_now);
#endif

    // Format the time into a string
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d_%H-%M-%S");

    return oss.str();
}

// Multiplatform function to get the sysroot path
std::string getSysRoot() {
#if defined(__APPLE__)
    try {
        // macOS: Use xcrun to get the SDK path
        //std::string sdkPath = execCommand("xcrun --sdk macosx --show-sdk-path");
        std::string sdkPath = exec("xcrun --sdk macosx --show-sdk-path", "", "GetSDK", true);
        // Remove any trailing newline or whitespace
        sdkPath.erase(sdkPath.find_last_not_of(" \n\r\t")+1);
        return sdkPath;
    } catch (const std::exception& e) {
        std::cerr << "Error retrieving macOS SDK path: " << e.what() << std::endl;
        return "";
    }

#elif defined(_WIN32) || defined(_WIN64)
    // Windows: Retrieve the Windows SDK directory from environment variables
    const char* sdkEnv = std::getenv("WindowsSdkDir");  
    if (sdkEnv) {
        std::string sdkPath(sdkEnv);
        // Ensure the path ends with a backslash
        if (!sdkPath.empty() && sdkPath.back() != '\\') {
            sdkPath += "\\";
        }
        return sdkPath;
    } else {
        std::cerr << "WindowsSdkDir environment variable not found." << std::endl;
        return "";
    }

#elif defined(__linux__)
    // Linux: Return "/" as the default sysroot, or check for CMAKE_SYSROOT
    const char* cmakeSysroot = std::getenv("CMAKE_SYSROOT");
    if (cmakeSysroot) {
        return std::string(cmakeSysroot);
    } else {
        return "/";
    }

#else
    // Unsupported platform
    std::cerr << "Unsupported platform." << std::endl;
    return "";
#endif
}

std::string getClangResourceDir() {
    std::string dir = exec("xcrun clang -print-resource-dir", "", "GetClangResourceDir", true);
    // trim trailing whitespace/newlines
    dir.erase(dir.find_last_not_of(" \t\r\n") + 1);
    return dir; // e.g. .../usr/lib/clang/17
}

std::string getClangInclude() {
    std::string r = getClangResourceDir();
    return r + "/include"; // e.g. .../usr/lib/clang/17/include
}

#if 0
std::string getCppInclude()
{
    std::string cppIncludePath = exec("echo $(xcrun --sdk macosx --show-sdk-path)/usr/include/c++/v1", "", "GetCppDir", true);
    return cppIncludePath;
}
#else
std::string getCppInclude() {
    // Get Clang's resource dir (toolchain path), e.g. .../usr/lib/clang/17
    std::string resource = exec("xcrun clang -print-resource-dir", "", "GetClangResourceDir", true);
    // trim trailing whitespace/newlines
    if (!resource.empty()) resource.erase(resource.find_last_not_of(" \t\r\n") + 1);

    // Simple parent-dir lambda (C++14)
    auto parent_dir = [](const std::string& p) -> std::string {
        const auto pos = p.find_last_of('/');
        return (pos == std::string::npos) ? std::string() : p.substr(0, pos);
    };

    // Walk up: .../usr/lib/clang/17  ->  .../usr/lib  ->  .../usr
    const std::string usrLib      = parent_dir(resource);       // .../usr/lib/clang
    const std::string usr         = parent_dir(usrLib);         // .../usr/lib
    const std::string toolchainUsr= parent_dir(usr);            // .../usr

    // libc++ headers live in the toolchain: .../usr/include/c++/v1
    return toolchainUsr + "/include/c++/v1";
}
#endif

std::string getExePath() {
#if defined(_WIN32)
   char path[MAX_PATH];
   GetModuleFileNameA(NULL, path, MAX_PATH);
   return std::string(path);
#elif defined(__APPLE__)
   char path[PATH_MAX];
   uint32_t size = sizeof(path);
   if (_NSGetExecutablePath(path, &size) == 0) {
       return std::string(path);
   }
   return "";
#else
   char path[PATH_MAX];
   ssize_t count = readlink("/proc/self/exe", path, PATH_MAX);
   return std::string(path, (count > 0) ? count : 0);
#endif
}


//https://claude.ai/chat/57e2ae4a-54ec-4b02-ae98-9a2ea08e48c4
std::string getSDKVersion() {
#if defined(__APPLE__)
    try {
        std::string sdkPath = getSysRoot();
        if (sdkPath.empty()) return "";
        
        // Execute xcrun with this specific SDK path to get its version
        std::string cmd = "xcodebuild -sdk \"" + sdkPath + "\" -version SDKVersion";
        std::string version = exec(cmd.c_str(), "", "GetSDKVersion", true);
        version.erase(version.find_last_not_of(" \n\r\t")+1);
        return version;
    } catch (const std::exception& e) {
        std::cerr << "Error retrieving macOS SDK version: " << e.what() << std::endl;
        return "";
    }

#elif defined(_WIN32) || defined(_WIN64)
    try {
        std::string sdkPath = getSysRoot();
        if (sdkPath.empty()) return "";

        // Windows SDK stores version info in sdk.h
        std::string versionHeaderPath = sdkPath + "Include\\um\\winsdkver.h";
        std::ifstream versionFile(versionHeaderPath);
        if (!versionFile.is_open()) {
            throw std::runtime_error("Cannot open winsdkver.h");
        }

        std::string line;
        std::string version;
        while (std::getline(versionFile, line)) {
            // Look for NTDDI_WIN10_FE or similar version defines
            if (line.find("#define WINVER_MAXVER") != std::string::npos) {
                size_t pos = line.find("0x");
                if (pos != std::string::npos) {
                    // Convert hex version to readable format
                    unsigned long ver;
                    std::stringstream ss;
                    ss << std::hex << line.substr(pos + 2);
                    ss >> ver;
                    
                    // Format: major.minor.build
                    unsigned long major = (ver >> 24) & 0xFF;
                    unsigned long minor = (ver >> 16) & 0xFF;
                    unsigned long build = ver & 0xFFFF;
                    
                    std::stringstream version_ss;
                    version_ss << major << "." << minor << "." << build;
                    version = version_ss.str();
                    break;
                }
            }
        }
        return version;
    } catch (const std::exception& e) {
        std::cerr << "Error retrieving Windows SDK version: " << e.what() << std::endl;
        return "";
    }

#elif defined(__linux__)
    try {
        std::string sysroot = getSysRoot();
        if (sysroot.empty() || sysroot == "/") {
            // When using system headers directly
            std::string version = exec("gcc -dumpversion", "", "GetSDKVersion", true);
            version.erase(version.find_last_not_of(" \n\r\t")+1);
            return version;
        }
        
        // For custom sysroot, try to read gcc's version file in the sysroot
        std::string versionPath = sysroot + "/usr/lib/gcc-version.h";
        std::ifstream versionFile(versionPath);
        if (versionFile.is_open()) {
            std::string line;
            while (std::getline(versionFile, line)) {
                if (line.find("__VERSION__") != std::string::npos) {
                    size_t start = line.find("\"");
                    size_t end = line.find("\"", start + 1);
                    if (start != std::string::npos && end != std::string::npos) {
                        return line.substr(start + 1, end - start - 1);
                    }
                }
            }
        }
        
        throw std::runtime_error("Unable to determine headers version in sysroot");
    } catch (const std::exception& e) {
        std::cerr << "Error retrieving system headers version: " << e.what() << std::endl;
        return "";
    }

#else
    std::cerr << "Unsupported platform." << std::endl;
    return "";
#endif
}

// Function to replace disallowed characters with ' '
std::string replaceDisallowedChars(const std::string& input) {
    // Define a regex that matches any character not alphanumeric, '-', '_', or space ' '
    std::regex disallowed("[^A-Za-z0-9_\\- ]");
    // Replace all disallowed characters with ' '
    return std::regex_replace(input, disallowed, " ");
}

std::string filterLinesByPattern(const std::string& text, const std::string& pattern) {
    std::regex re(pattern);
    std::stringstream result;
    std::stringstream ss(text);
    std::string line;
    
    while (std::getline(ss, line)) {
        if (std::regex_search(line, re)) {
            result << line << '\n';
        }
    }
    
    return result.str();
}

std::pair<std::string, std::string> splitByFirstOccurence(const std::string& str, char delimiter) {
    size_t pos = str.find(delimiter);
    if (pos == std::string::npos) {
        return {str, ""};
    }
    return {str.substr(0, pos), str.substr(pos + 1)};
}

std::string makeCanonical(const std::string& path) {
    boost_fs::path p(path);
    boost_fs::path result = p.is_absolute() ? "/" : "";
    for (const boost_fs::path& part : p) {
        if (part == "..") {
            result = result.parent_path();
        } else if (part != "." && part != "/") {
            result /= part;
        }
    }
    return result.string();
}

std::string getTargetFromUrl(const std::string &urlString)
{
    auto result = boost::urls::parse_uri(urlString);
    if (!result)
    {
        std::cerr << "Error parsing URL: " << result.error().message() << "\n";
        return {};
    }
    boost::urls::url_view uv = *result;

    // Path plus optional query
    std::string target = std::string(uv.encoded_path());
    if (uv.has_query()) {
        target += "?";
        target += std::string(uv.encoded_query());
    }
    return target;
}


#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <iostream>
#include <stdexcept>

std::string zipDirectory(
    const std::string& dirPath,
    const std::string& customZipName = "",
    const std::string& password = ""
) {
    namespace fs = boost::filesystem;

    fs::path path(dirPath);

    // Debug prints
    std::cout << "Input path: " << dirPath << std::endl;
    std::cout << "Canonical path: " << fs::canonical(path).string() << std::endl;
    std::cout << "Exists: " << fs::exists(path) << std::endl;
    std::cout << "Is directory: " << fs::is_directory(path) << std::endl;
    
    // Check if directory exists
    if (!fs::exists(path) || !fs::is_directory(path)) {
        throw std::runtime_error("Invalid directory path: " + path.string());
    }

    // Create zip file path using custom name if provided, otherwise use directory name
    fs::path zipPath;
    if (customZipName.empty()) {
        zipPath = path.parent_path() / (path.filename().string() + ".zip");
    } else {
        // Ensure the custom name ends with .zip
        std::string zipName = customZipName;
        if (zipName.size() < 4 || zipName.substr(zipName.size() - 4) != ".zip") {
            zipName += ".zip";
        }
        zipPath = path.parent_path() / zipName;
    }

    // Ensure the parent directory of the zip file exists
    fs::create_directories(zipPath.parent_path());

    std::string command;

#ifdef _WIN32
    // On Windows, PowerShell's Compress-Archive does not support a password, so we use 7-Zip if a password is needed.
    if (password.empty()) {
        command = "powershell -command \"Compress-Archive -Path '"
                  + path.string() + "\\*' -DestinationPath '"
                  + zipPath.string() + "' -Force\"";
    } else {
        // 7z must be installed and available on PATH
        command = "7z a -tzip -p\"" + password + "\" \""
                  + zipPath.string() + "\" \""
                  + path.string() + "\\*\"";
    }
#else
    // On Unix-like systems, we’ll do something like:
    //    zip -r [-P password] /path/to/result.zip /absolute/or/relative/path
    //
    // We also specify the “start_dir” for the process to be the directory
    // we want to compress (so we can just zip “.” if you prefer).
    // Or we can zip the absolute path directly (less confusion).
    //
    // For example:
    //    zip -r /Users/.../myzipfile.zip .
    // or
    //    zip -r -P mypassword /Users/.../myzipfile.zip .
    //
    // Either works as long as you set start_dir = path.string().
    // Let’s just zip “.” to capture everything in that folder.

    if (password.empty()) {
        // e.g.: zip -r /path/to/output.zip .
        command = "zip -r \"" + zipPath.string() + "\" .";
    } else {
        // e.g.: zip -r -P MySecretPassword /path/to/output.zip .
        // Putting -P after -r is more standard: zip -r -P pass zipfile .
        command = "zip -r -P \"" + password + "\" \"" + zipPath.string() + "\" .";
    }
#endif

    std::cout << "Executing command: " << command << std::endl;

    try {
        // Parse command line (assuming `command` is a string)
        std::vector<std::string> tokens = parseCommandLine(command);
        
        if (tokens.empty()) {
            throw std::runtime_error("Empty command");
        }
        
        std::string executable = tokens[0];
        std::vector<std::string> args(tokens.begin() + 1, tokens.end());

        // Create io_context (required for v2)
        boost::asio::io_context ctx;

        // Create process with working directory set to `path`
        // so that "." refers to the contents of `dirPath`.
        boost_prc::process c(
            ctx,
            executable,
            args,
            boost_prc::process_start_dir(path.string())
        );
        c.wait();

        // Check if zip file was created
        if (!fs::exists(zipPath)) {
            throw std::runtime_error("Zip file was not created");
        }
    } catch (const boost::system::system_error& e) {
        throw std::runtime_error("Failed to create zip file: " + std::string(e.what()));
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to create zip file: " + std::string(e.what()));
    }
    
    return zipPath.string();
}

std::string generateApiKey()
{
    static boost::uuids::random_generator_mt19937 gen;
    boost::uuids::uuid u = gen();
    // Convert UUID object to string
    return "gkr_" + boost::uuids::to_string(u);
}

std::string removeThinkPart(const std::string& input)
{
    std::size_t thinkStart = input.find("<think>");
    std::size_t thinkEnd = input.find("</think>");

    // If both tags are found, remove the <think> part
    if (thinkStart != std::string::npos && thinkEnd != std::string::npos) {
        // Calculate the length of the <think> part including the tags
        std::size_t thinkLength = thinkEnd - thinkStart + std::string("</think>").length();

        // Remove the <think> part from the string
        std::string result = input;
        result.erase(thinkStart, thinkLength);
        return result;
    }

    // If the tags are not found, return the original string
    return input;
}

std::ifstream openFile(const std::string& fileName, const std::set<std::string>& searchPaths)
{
    namespace fs = std::filesystem;

    boost_fs::path filePath(fileName);

    // 1. If absolute path, just try to open
    if (filePath.is_absolute()) {
        std::ifstream ifs(filePath.string(), std::ios::in | std::ios::binary);
        if (ifs.is_open()) {
            return ifs; // Successfully opened
        }
    }
    else {
        // 2. Otherwise, try current directory first
        {
            std::ifstream ifs(filePath.string(), std::ios::in | std::ios::binary);
            if (ifs.is_open()) {
                return ifs; // Successfully opened in current directory
            }
        }

        // 3. Otherwise, try each directory in the search path
        for (const auto& directory : searchPaths) {
            boost_fs::path candidate = boost_fs::path(directory) / filePath;
            std::ifstream ifs(candidate.string(), std::ios::in | std::ios::binary);
            if (ifs.is_open()) {
                return ifs; // Successfully opened from a search directory
            }
        }
    }

    // 4. If nothing worked, return a default-constructed ifstream (not open)
    return std::ifstream{};
}

std::string getFileContent(const std::string& fileName, const std::set<std::string>& searchPaths)
{
    std::ifstream file = openFile(fileName, searchPaths);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

boost::optional<std::string> getFirstLine(const std::string& fileName, const std::set<std::string>& searchPaths)
{
    std::ifstream file = openFile(fileName, searchPaths);
    if (!file) {
        return boost::none;   // file missing → return "null"
    }

    std::string line;
    if (!std::getline(file, line)) {
        return std::string();
    }

    // Trim whitespace
    auto trim = [](std::string& s) {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { s.clear(); return; }
        auto end = s.find_last_not_of(" \t\r\n");
        s = s.substr(start, end - start + 1);
    };

    trim(line);
    return line;               // return optional<string> containing the value
}

bool loadJson(web::json::value& json, const std::string& path)
{
    if (!boost_fs::exists(path))
        return false;

    std::ifstream file(path);
    if (!file) {
        return false;
    }

    std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    if (str.empty()) {
        return false;
    }

    auto ustr = conversions::to_string_t(str);

    try {
        json = json::value::parse(ustr);
    }
    catch (const std::exception& e) {
        //std::cout << "JSON parsing failed: " << e.what() << std::endl;
        return false;
    }

    return true;
}

bool saveToFile(const std::string& content, const std::string& path)
{
    auto directory = boost_fs::path(path).parent_path();
    if(!boost_fs::exists(directory))
    {
        boost_fs::create_directories(directory);
    }
    
    std::ofstream fileJson(path);
    if (!fileJson.is_open()) {
        std::cout << "Unable to create file " << path << std::endl;
        return false;
    }

    fileJson << content;
    
    fileJson.close();
    return true;
}

bool saveJson(const web::json::value& json, const std::string& path)
{
    std::string strJson = conversions::to_utf8string(json.serialize());
    return saveToFile(strJson, path);
}

// This function filters the input string so that it:
// 1. Converts non-ASCII characters to a placeholder (here we use '?')
// 2. Ensures that every backslash is followed only by an allowed escape character.
//    If not, it prefixes the backslash with another backslash.
std::string filterJsonText(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = input[i];
        
        // Convert non-ASCII characters to a placeholder, e.g. '?'
        if (c > 127) {
            output.push_back('?');
            continue;
        }
        
        if (c == '\\') {
            // If backslash is the last character, double it.
            if (i + 1 >= input.size()) {
                output.append("\\\\");
                break;
            }
            
            char next = input[i + 1];
            bool allowedEscape = false;
            
            // Allowed simple escape characters: " \ / b f n r t
            if (next == '\"' || next == '\\' || next == '/' ||
                next == 'b'  || next == 'f'  || next == 'n' ||
                next == 'r'  || next == 't') {
                allowedEscape = true;
            }
            // Allowed Unicode escape: must be 'u' followed by four hex digits.
            else if (next == 'u' && i + 5 < input.size()) {
                bool validHex = true;
                for (size_t j = i + 2; j <= i + 5; ++j) {
                    char h = input[j];
                    if (!((h >= '0' && h <= '9') ||
                          (h >= 'A' && h <= 'F') ||
                          (h >= 'a' && h <= 'f'))) {
                        validHex = false;
                        break;
                    }
                }
                allowedEscape = validHex;
            }
            
            // If not a valid escape, prefix the backslash with another backslash.
            if (!allowedEscape) {
                output.append("\\\\");
            }
            
            // Always output the backslash and then the next character.
            output.push_back('\\');
            ++i;  // Skip over the next character since we processed it.
            output.push_back(input[i]);
        }
        else {
            // Output the character as is.
            output.push_back(c);
        }
    }
    
    return output;
}

bool isBinaryFile(const std::string& filePath) {
    // Open the file in binary mode.
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Unable to open file: " + filePath);
    }

    // Read a sample from the file (up to 1024 bytes)
    const std::size_t sampleSize = 1024;
    char buffer[sampleSize];
    file.read(buffer, sampleSize);
    std::streamsize bytesRead = file.gcount();

    // An empty file is considered text.
    if (bytesRead == 0) {
        return false;
    }

    int nonTextCount = 0;
    for (std::streamsize i = 0; i < bytesRead; ++i) {
        unsigned char c = buffer[i];
        // If we encounter a null character, consider it binary.
        if (c == '\0') {
            return true;
        }
        // Check for non-printable characters.
        // Allow common whitespace: newline, carriage return, tab.
        if ((c < 32 || c > 126) && c != '\n' && c != '\r' && c != '\t') {
            ++nonTextCount;
        }
    }

    // If more than 30% of the characters are non-text, consider it binary.
    double ratio = static_cast<double>(nonTextCount) / bytesRead;
    return (ratio > 0.30);
}

std::string readTextFileWithLimit(const std::string &filePath, std::size_t maxBytes)
{
    namespace fs = boost::filesystem;

    // If the file doesn't exist or isn't a regular file, return empty string
    if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
        return {};
    }

    // Open the file in 'text mode'. On macOS, there's no special newline translation
    std::ifstream ifs(filePath);
    if (!ifs) {
        // Could not open file, return empty string or handle error
        return {};
    }

    // We'll read in chunks
    static constexpr std::size_t BUFFER_SIZE = 4096;
    std::string result;
    result.reserve(std::min<std::size_t>(maxBytes, BUFFER_SIZE)); // Minor optimization

    char buffer[BUFFER_SIZE];
    std::size_t totalRead = 0;

    while (ifs && totalRead < maxBytes)
    {
        // We only want to read the remaining allowed bytes if less than BUFFER_SIZE is left
        std::size_t bytesToRead = std::min(BUFFER_SIZE, maxBytes - totalRead);

        // Read up to bytesToRead characters into the buffer
        ifs.read(buffer, static_cast<std::streamsize>(bytesToRead));
        std::streamsize justRead = ifs.gcount();

        if (justRead > 0)
        {
            // Append what was read to our string
            result.append(buffer, static_cast<std::size_t>(justRead));
            totalRead += static_cast<std::size_t>(justRead);
        }
    }

    return result;
}

std::string printLineNumbers(const std::string& text, int lineOffset)
{
    std::istringstream iss(text);
    std::string output;
    
    uint32_t lineNumber = 1;
    std::string line;
    
    while (std::getline(iss, line))
    {
        std::ostringstream oss;
        // Use left alignment with a field width of 5
        oss << std::left << std::setw(5) << (lineOffset + lineNumber) << " " << line << "\n";
        
        output += oss.str();
        ++lineNumber;
    }
    
    return output;
}

// Returns a new string with all newline variations normalized to "\n".
std::string normalizeNewLines(const std::string& input) {
    return std::regex_replace(input, std::regex("\r\n|\r"), "\n");
}

int normalizeAndMapLine(const std::string& from, const std::string& to, int lineNumber)
{
    auto A = normalizeNewLines(from);
    auto B = normalizeNewLines(to);
    
    return mapLine(A, B, lineNumber);
}

int mapLine(const std::string& from, const std::string& to, int lineNumber)
{
    auto& A = from;
    auto& B = to;
    
    // Find A in B
    size_t pos = B.find(A);
    if (pos == std::string::npos) {
        // A is not fully contained in B.
        return -1;
    }

    int baseLine = 0;
    for (size_t i = 0; i < pos; ++i) {
        if (B[i] == '\n')
            ++baseLine;
    }

    // Count total lines in A.
    int totalLines = 1;
    for (char c : A) {
        if (c == '\n')
            ++totalLines;
    }
    
    // Check if the requested line number in A is valid.
    if (lineNumber < 1 || lineNumber > totalLines)
        return -1;
    
    // The mapping: the first line of A maps to baseLine in B,
    // so line 'lineNumber' in A maps to (baseLine + (lineNumber - 1)).
    return baseLine + (lineNumber - 1);
}

//returns the modified text with inserted snippet and the character position at the line from where the inserted snippet starts
//If lineNumber is < 1 or bigger that the number of the lines in text the returned string must be empty and the returned integer must be -1
std::pair<std::string, int> insertSnippet(const std::string& text,      // The original text
                                          const std::string& snippet,   // The snippet to insert
                                          int lineNumber,               // 1-based line number at which to insert
                                          bool skipWhitespaces)         // Skip leading whitespaces on that line
{
    // If the requested line number is invalid, return empty results.
    if (lineNumber < 1)
        return std::make_pair(std::string(""), -1);
    // Compute the start indices for each line.
    std::vector<size_t> lineStartIndices;
    lineStartIndices.push_back(0);  // The first line always starts at index 0.
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && i + 1 < text.size()) {
            lineStartIndices.push_back(i + 1);
        }
    }
    // If the line number is beyond the available lines, return empty results.
    if (static_cast<size_t>(lineNumber) > lineStartIndices.size())
        return std::make_pair(std::string(""), -1);
    // Find the starting index of the requested line.
    size_t insertionIndex = lineStartIndices[lineNumber - 1];
    // Calculate column offset from the start of the line (1-based)
    int columnOffset = 1;
    
    // If skipping whitespaces is requested, move the insertion index to the first non-whitespace.
    if (skipWhitespaces) {
        while (insertionIndex < text.size() && (text[insertionIndex] == ' ' || text[insertionIndex] == '\t')) {
            ++insertionIndex;
            ++columnOffset;
        }
    }
    // Build the modified text by inserting the snippet at the computed insertion index.
    std::string modifiedText = text.substr(0, insertionIndex) + snippet + text.substr(insertionIndex);
    
    return std::make_pair(modifiedText, columnOffset);
}

// Inserts string B into string A at the specified 1-based line and column.
// Returns a pair containing the modified string and a boolean flag indicating whether
// both the line and column were within the original string's ranges.
// If either the line or column is out-of-range, the function returns an empty string and false.
std::pair<std::string, bool> insertStringAt(const std::string& A, const std::string& B,
                                            uint32_t line, uint32_t col) {
    // Split A into lines based on newline characters.
    std::vector<std::string> lines;
    boost::split(lines, A, boost::is_any_of("\n"));

    // Check that the provided line number is within range.
    // Since line is 1-based, it should be at least 1 and no greater than the number of lines.
    if (line == 0 || line > lines.size()) {
        return { "", false };
    }

    // Convert 1-based line index to 0-based.
    std::size_t lineIndex = line - 1;

    // For a valid insertion in a line, the 1-based column should be in the range [1, lineLength+1].
    std::size_t lineLength = lines[lineIndex].size();
    if (col == 0 || col > lineLength + 1) {
        return { "", false };
    }
    
    // Convert 1-based column to 0-based insertion position.
    std::size_t insertionPos = col - 1;
    
    // Insert B into the target line.
    lines[lineIndex].insert(insertionPos, B);
    
    // Reassemble the string using newline characters.
    std::string result = boost::join(lines, "\n");
    return { result, true };
}

std::size_t getFileHash(const std::string& filePath)
{
    // 1) Open in binary mode to avoid any newline translations
    std::ifstream ifs(filePath, std::ios::in | std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("Failed to open file: " + filePath);
    }

    // 2) Read entire file into a string, reserving up front
    ifs.seekg(0, std::ios::end);
    std::streamsize fileSize = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::string contents;
    contents.reserve(static_cast<std::size_t>(fileSize));
    contents.assign((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());

    // 3) Hash the full range of bytes
    return boost::hash_range(contents.begin(), contents.end());
}

bool isTextOnlyFile(const std::string& path)
{
    if (!boost_fs::exists(path) || !boost_fs::is_regular_file(path))
        return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    // Read in 4KB chunks for better performance
    constexpr size_t CHUNK_SIZE = 4096;
    char buffer[CHUNK_SIZE];
    
    while (in.read(buffer, CHUNK_SIZE) || in.gcount() > 0) {
        size_t bytesRead = in.gcount();
        
        for (size_t i = 0; i < bytesRead; ++i) {
            unsigned char uc = static_cast<unsigned char>(buffer[i]);
            
            // Allow: tab, LF, FF, CR, and printable ASCII
            if (uc == 0x09 || uc == 0x0A || uc == 0x0C || uc == 0x0D)
                continue;
            if (uc >= 0x20 && uc <= 0x7E)
                continue;
                
            return false;  // Non-ASCII character found
        }
    }
    
    return true;
}

static bool decodeOneUtf8(const unsigned char* s, size_t len, size_t& adv)
{
    adv = 0;
    if (len == 0) return true;

    const unsigned char b0 = s[0];

    // ASCII: allow your whitespace + printable ASCII
    if (b0 < 0x80) {
        adv = 1;
        if (b0 == 0x09 || b0 == 0x0A || b0 == 0x0B || b0 == 0x0C || b0 == 0x0D) return true; // \t \n \v \f \r
        if (b0 >= 0x20 && b0 <= 0x7E) return true;
        return false;
    }

    // Multibyte UTF-8 (reject overlongs and invalid ranges)
    auto need = [&](size_t n) { return len >= n; };
    auto cont = [&](unsigned char b) { return (b & 0xC0) == 0x80; };

    if (b0 >= 0xC2 && b0 <= 0xDF) {                    // 2-byte
        if (!need(2) || !cont(s[1])) return false;
        adv = 2; return true;
    }
    if (b0 == 0xE0) {                                  // 3-byte, special lower bound
        if (!need(3) || !(s[1] >= 0xA0 && s[1] <= 0xBF) || !cont(s[2])) return false;
        adv = 3; return true;
    }
    if (b0 >= 0xE1 && b0 <= 0xEC) {                    // 3-byte
        if (!need(3) || !cont(s[1]) || !cont(s[2])) return false;
        adv = 3; return true;
    }
    if (b0 == 0xED) {                                  // 3-byte, avoid UTF-16 surrogates
        if (!need(3) || !(s[1] >= 0x80 && s[1] <= 0x9F) || !cont(s[2])) return false;
        adv = 3; return true;
    }
    if (b0 >= 0xEE && b0 <= 0xEF) {                    // 3-byte
        if (!need(3) || !cont(s[1]) || !cont(s[2])) return false;
        adv = 3; return true;
    }
    if (b0 == 0xF0) {                                  // 4-byte, special lower bound
        if (!need(4) || !(s[1] >= 0x90 && s[1] <= 0xBF) || !cont(s[2]) || !cont(s[3])) return false;
        adv = 4; return true;
    }
    if (b0 >= 0xF1 && b0 <= 0xF3) {                    // 4-byte
        if (!need(4) || !cont(s[1]) || !cont(s[2]) || !cont(s[3])) return false;
        adv = 4; return true;
    }
    if (b0 == 0xF4) {                                  // 4-byte, max U+10FFFF
        if (!need(4) || !(s[1] >= 0x80 && s[1] <= 0x8F) || !cont(s[2]) || !cont(s[3])) return false;
        adv = 4; return true;
    }

    return false;
}

bool isTextFileAsciiOrUtf8(const std::string& path)
{
    const boost_fs::path p(path);
    if (!boost_fs::exists(p) || !boost_fs::is_regular_file(p))
        return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    constexpr size_t CHUNK_SIZE = 4096;
    std::array<unsigned char, CHUNK_SIZE> buf{};

    // Handle optional UTF-8 BOM at start: EF BB BF
    bool firstChunk = true;

    std::array<unsigned char, 4> carry{};
    size_t carryLen = 0;

    while (in.read(reinterpret_cast<char*>(buf.data()), buf.size()) || in.gcount() > 0) {
        size_t n = static_cast<size_t>(in.gcount());

        size_t start = 0;
        if (firstChunk) {
            firstChunk = false;
            if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
                start = 3; // skip UTF-8 BOM
            }
        }

        // Combine carry + current chunk (from `start`)
        std::vector<unsigned char> data;
        data.reserve(carryLen + (n - start));
        data.insert(data.end(), carry.begin(), carry.begin() + carryLen);
        data.insert(data.end(), buf.begin() + start, buf.begin() + n);

        // Quick binary signal: NUL byte
        for (unsigned char b : data) {
            if (b == 0x00) return false; // (will still reject UTF-16/32, by design)
        }

        size_t i = 0;
        while (i < data.size()) {
            size_t adv = 0;
            if (!decodeOneUtf8(data.data() + i, data.size() - i, adv))
                return false;
            if (adv == 0) break; // incomplete sequence at end
            i += adv;
        }

        carryLen = data.size() - i;
        if (carryLen > 3) return false; // should never happen with UTF-8
        for (size_t k = 0; k < carryLen; ++k) carry[k] = data[i + k];
    }

    // No dangling partial UTF-8 sequence
    return (carryLen == 0);
}


bool parsePrefixFlags(const std::string& s,
                      bool& debug,
                      bool& checkResult,
                      std::string& expectedResult,
                      std::string& stdOutRegex,
                      std::string& stripped)
{
    // local helpers
    auto trim = [](std::string& t) {
        auto notsp = [](unsigned char ch){ return !std::isspace(ch); };
        t.erase(t.begin(), std::find_if(t.begin(), t.end(), notsp));
        t.erase(std::find_if(t.rbegin(), t.rend(), notsp).base(), t.end());
    };

    auto unquote = [&](std::string& x) {
        trim(x);
        if (x.size() >= 2) {
            char a = x.front(), b = x.back();
            if ((a=='"' && b=='"') || (a=='\'' && b=='\'')) {
                x = x.substr(1, x.size() - 2);
            }
        }
    };

    debug = false;
    checkResult = false;
    expectedResult.clear();
    stdOutRegex.clear();                                        // <- NEW: reset stdout regex
    stripped = s;

    // must start with [[
    if (s.size() < 4 || s[0] != '[' || s[1] != '[') return false;

    const std::string::size_type close = s.find("]]", 2);
    if (close == std::string::npos) return false;

    std::string header = s.substr(2, close - 2);

    std::istringstream iss(header);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        trim(tok);

        if (tok == "debug") {
            debug = true;
            continue;
        }

        // Accept "result" or "result = <string>"
        if (tok.rfind("result", 0) == 0) { // starts with "result"
            checkResult = true;

            const auto eq = tok.find('=');
            if (eq != std::string::npos) {
                std::string val = tok.substr(eq + 1);
                trim(val);
                unquote(val);             // handle "..." or '...'
                expectedResult = val;      // may be empty if "result="
            }
            continue;
        }

        // Accept "stdout = <string>" (captures regex/text to match on stdout)
        if (tok.rfind("stdout", 0) == 0) { // starts with "stdout"
            checkResult = true;
            
            const auto eq = tok.find('=');
            if (eq != std::string::npos) {
                std::string val = tok.substr(eq + 1);
                trim(val);
                unquote(val);             // handle "..." or '...'
                stdOutRegex = val;        // may be empty if "stdout="
            }
            // bare "stdout" (no '=') is ignored; add semantics if you need it later
            continue;
        }
    }

    // everything after the closing ]] is the actual command
    stripped = s.substr(close + 2);
    trim(stripped);
    return true;
}

std::string makeTestCommand(const std::string& cmd,
                            bool debug,
                            bool checkResult,
                            const std::string& expectedResult,
                            const std::string& stdoutRegex)
{
    std::string newCmd;
    if(debug || checkResult || !stdoutRegex.empty())
    {
        newCmd += "[[";
        
        std::string attributes;
        if(debug)
        {
            if(!attributes.empty()) attributes += ", ";
            attributes += "debug";
        }
        
        if(checkResult)
        {
            if(!attributes.empty()) attributes += ", ";
            attributes += "result=" + expectedResult;
        }
        
        if(!stdoutRegex.empty())
        {
            if(!attributes.empty()) attributes += ", ";
            attributes += "stdout=" + stdoutRegex;
        }
        
        if(!attributes.empty())
        {
            newCmd += attributes;
        }
        
        newCmd += "]]";
    }
    
    newCmd += cmd;
    
    return newCmd;
}

bool tryMakeRegex(const std::string& pattern, std::regex& out,
                            std::regex_constants::syntax_option_type flags,
                            std::string* error) noexcept
{
    try {
        out.assign(pattern, flags);                   // compiles or throws
        return true;
    } catch (const std::regex_error& e) {
        if (error) *error = e.what();                // optional diagnostic
        out = std::regex();                          // reset to a safe state
        return false;
    }
}

std::pair<std::string, int32_t> makeStringNumberPair(const std::string& pairStr) {
    auto colon = pairStr.find_last_of(':');
    return (colon == std::string::npos)
             ? std::make_pair(pairStr, uint32_t(0))
             : std::make_pair(pairStr.substr(0, colon), uint32_t(std::atoi(pairStr.substr(colon + 1).c_str())));
}

// Find a directory named `dirName` anywhere under `root`.
// - Returns the absolute path if found, or an empty path if not found.
// - If a second directory with the same name is encountered, throws.
boost_fs::path findDirectoryByName(const boost_fs::path& root,
                                          const std::string& dirName)
{
    if (!boost_fs::exists(root) || !boost_fs::is_directory(root)) {
        throw std::runtime_error("findDirectoryByName: root is not a directory: " + root.string());
    }

    boost::system::error_code ec;
    boost_fs::path found; // empty = not found yet

    // Check the root itself (in case the root's last component matches).
    if (root.filename() == dirName) {
        found = boost_fs::canonical(root, ec);
        if (ec) found = root; // fall back if canonical fails
    }

    // Walk recursively
    for (boost_fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; ) {
        const boost_fs::path p = it->path();

        // Safely query status (avoid exceptions)
        boost::system::error_code sec;
        const bool isDir = boost_fs::is_directory(it->status(sec));
        if (!sec && isDir) {
            if (p.filename() == dirName) {
                if (found.empty()) {
                    // First match
                    found = boost_fs::canonical(p, sec);
                    if (sec) found = p; // fall back if canonical fails
                } else {
                    // Second match -> violate uniqueness guarantee
                    throw std::runtime_error(
                        "findDirectoryByName: multiple directories named '" + dirName +
                        "' under root '" + root.string() + "':\n- " + found.string() +
                        "\n- " + p.string());
                }
            }
        }

        // Advance iterator with error_code (keeps loop resilient on permission issues)
        it.increment(ec);
        if (ec) {
            // Clear and continue past problematic entries
            ec.clear();
        }
    }

    return found;
}

static std::string normalizeNewlines(std::string s)
{
    // Convert CRLF -> LF
    // Convert lone CR -> LF
    std::string out;
    out.reserve(s.size());

    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\r') {
            // If this is CRLF, skip the LF
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
            out.push_back('\n');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool fullRegexMatch(const std::string& logRaw,
                    const std::string& pattern,
                    std::string& err)
{
    const std::string log = normalizeNewlines(logRaw);

    try {
        std::regex re(pattern, std::regex::ECMAScript);
        return std::regex_match(log, re); // full-string match
    } catch (const std::regex_error& e) {
        err = e.what();
        return false;
    }
}

namespace {

bool isEmptyAnthropicTextBlock(const json::value& content)
{
    if(!content.is_object() ||
       !content.has_field(U("type")) ||
       !content.at(U("type")).is_string())
    {
        return false;
    }

    if(content.at(U("type")).as_string() != U("text"))
    {
        return false;
    }

    return !content.has_field(U("text")) ||
           !content.at(U("text")).is_string() ||
           content.at(U("text")).as_string().empty();
}

void appendAnthropicContentBlock(json::value& contentBlocks, const json::value& content)
{
    if(content.is_string())
    {
        if(content.as_string().empty())
        {
            return;
        }

        json::value textBlock = json::value::object();
        textBlock[U("type")] = json::value::string(U("text"));
        textBlock[U("text")] = json::value::string(content.as_string());
        auto size = contentBlocks.size();
        contentBlocks[size] = textBlock;
        return;
    }
    
    if(content.is_array())
    {
        for(const auto& block : content.as_array())
        {
            appendAnthropicContentBlock(contentBlocks, block);
        }
        return;
    }
    
    if(content.is_object())
    {
        if(isEmptyAnthropicTextBlock(content))
        {
            return;
        }

        auto size = contentBlocks.size();
        contentBlocks[size] = content;
    }
}

}

void alternateRoles(json::value& body)
{
    uint32_t messagesCount = (uint32_t)body[U("messages")].as_array().size();
    
    string_t currentRole;
    json::value currentContent = json::value::array();
    auto messages = json::value::array();
    for(uint32_t i=0; i<messagesCount; ++i)
    {
        const auto& message = body[U("messages")].as_array()[i];
        auto role = message.at(U("role")).as_string();
        if(role != currentRole && !currentRole.empty())
        {
            if(currentContent.size() > 0)
            {
                json::value mergedMessage;
                mergedMessage[U("role")] = json::value::string(currentRole);
                mergedMessage[U("content")] = currentContent;
                
                auto size = messages.size();
                messages[size] = mergedMessage;
            }
            
            currentContent = json::value::array();
        }
        
        currentRole = role;
        appendAnthropicContentBlock(currentContent, message.at(U("content")));
    }
    
    if(!currentRole.empty() && currentContent.size() > 0)
    {
        json::value mergedMessage;
        mergedMessage[U("role")] = json::value::string(currentRole);
        mergedMessage[U("content")] = currentContent;
        
        auto size = messages.size();
        messages[size] = mergedMessage;
    }
    
    body[U("messages")] = messages;
}

static std::vector<std::string> tokenizeSimple(const std::string& s)
{
    std::vector<std::string> args;
    std::string cur;
    bool inQuotes = false;
    char quoteChar = 0;

    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];

        if (inQuotes) {
            if (c == quoteChar) {
                inQuotes = false;
            } else {
                // minimal escape handling inside quotes: \" or \' etc.
                if (c == '\\' && i + 1 < s.size()) {
                    cur += c;
                    cur += s[++i];
                } else {
                    cur += c;
                }
            }
        } else {
            if (c == '"' || c == '\'') {
                inQuotes = true;
                quoteChar = c;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) { args.push_back(cur); cur.clear(); }
            } else {
                cur += c;
            }
        }
    }

    if (!cur.empty()) args.push_back(cur);
    return args;
}

std::set<std::string> extractFilesFromCommandLine(const std::string& commandLine)
{
    auto args = tokenizeSimple(commandLine);

    auto hasDrivePrefix = [](const std::string& s) {
        return s.size() >= 2 &&
               std::isalpha(static_cast<unsigned char>(s[0])) &&
               s[1] == ':';
    };

    auto startsWithRel = [](const std::string& s) {
        return s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0;
    };

    auto hasExtension = [&](const std::string& s) {
        auto dot = s.find_last_of('.');
        if (dot == std::string::npos) return false;

        auto sep = s.find_last_of("/\\");
        if (sep != std::string::npos && dot < sep) return false;
        if (dot + 1 >= s.size()) return false;

        if (sep == std::string::npos) {
            if (dot == 0) return false; // ".gitignore"
        } else {
            if (dot == sep + 1) return false;
        }
        return true;
    };

    auto hasPathSep = [&](const std::string& s) {
        if (s.find('/') != std::string::npos) return true;

        if (s.find('\\') != std::string::npos) {
            if (hasDrivePrefix(s)) return true;        // C:\foo\bar
            if (s.rfind("\\\\", 0) == 0) return true;  // \\server\share
            if (hasExtension(s)) return true;          // foo\bar.txt
            return false;                              // likely escapes like \n
        }
        return false;
    };

    auto looksLikePath = [&](const std::string& s) {
        if (s.empty()) return false;
        if (s.size() > 1 && s[0] == '-') return false; // ignore flags
        return (startsWithRel(s) || hasDrivePrefix(s) || hasPathSep(s)) && hasExtension(s);
    };

    auto isRedir = [](const std::string& t) {
        return t == ">" || t == ">>" || t == "<" || t == "2>" || t == "2>>" || t == "1>" || t == "1>>" || t == "&>";
    };
    
    std::set<std::string> files;
    auto insertFile = [&](const std::string& f) {
        if(startsWith(f, "./"))
        {
            files.insert(boost_fs::path(f).filename().string());
        }
        else
        {
            files.insert(f);
        }
    };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];

        // Parse nested command line: sh -c "<here>"
        if (a == "-c" && i + 1 < args.size()) {
            auto nested = extractFilesFromCommandLine(args[i + 1]);
            for(auto file : nested)
            {
                insertFile(file);
            }
            ++i; // consume the nested string
            continue;
        }

        // If we see redirection, the next token is a file (even if it has no extension)
        if (isRedir(a) && i + 1 < args.size()) {
            const std::string& target = args[i + 1];
            if (!target.empty() && !(target.size() > 1 && target[0] == '-'))
                insertFile(target);
            ++i;
            continue;
        }

        if (looksLikePath(a))
        {
            insertFile(a);
        }
    }

    return files;
}

std::size_t nextIndex(const boost_fs::path& dir,
                                const std::string& prefix)
{
    auto parseSuffixNumber = [](const std::string& name,
                                const std::string& prefix,
                                std::size_t& out) -> bool
    {
        if (name.size() <= prefix.size()) return false;
        if (name.compare(0, prefix.size(), prefix) != 0) return false;

        const std::string suffix = name.substr(prefix.size());
        if (suffix.empty()) return false;

        if (!std::all_of(suffix.begin(), suffix.end(),
                         [](unsigned char c){ return std::isdigit(c); }))
            return false;

        try {
            out = static_cast<std::size_t>(std::stoull(suffix));
            return true;
        } catch (...) {
            return false;
        }
    };

    std::size_t maxN = 0;

    for (boost_fs::directory_iterator it(dir), end; it != end; ++it) {
        if (!boost_fs::is_directory(it->status())) continue;

        const std::string name = it->path().filename().string();
        std::size_t n = 0;
        if (parseSuffixNumber(name, prefix, n)) {
            maxN = std::max(maxN, n);
        }
    }

    return maxN + 1; // if none found => 1
}

// Extracts the first non-whitespace token from a shell-like command line.
// Examples:
//   "./feature_test arithmetic:5,2" -> "./feature_test"
//   "   /usr/bin/python3   script.py" -> "/usr/bin/python3"
// Note: does not try to parse quotes/escapes; it just returns the first token.
std::string extractExecutablePath(const std::string& cmdLine)
{
    std::size_t i = 0;
    const std::size_t n = cmdLine.size();

    // skip leading whitespace
    while (i < n && std::isspace(static_cast<unsigned char>(cmdLine[i]))) ++i;
    if (i == n) return "";

    // read until next whitespace
    const std::size_t start = i;
    while (i < n && !std::isspace(static_cast<unsigned char>(cmdLine[i]))) ++i;

    return cmdLine.substr(start, i - start);
}

size_t utf8CharLen(unsigned char c) {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0; // invalid lead byte
}

std::string utf8TruncateBytes(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;

    size_t i = 0;
    size_t lastGood = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = utf8CharLen(c);
        if (len == 0) break;                  // invalid UTF-8 lead byte
        if (i + len > s.size()) break;        // truncated source string
        if (i + len > maxBytes) break;        // would exceed maxBytes
        // Validate continuation bytes (optional but nice)
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { len = 0; break; }
        }
        if (len == 0) break;

        i += len;
        lastGood = i;
    }
    return s.substr(0, lastGood);
}

std::string truncateWithNoteUtf8(const std::string& s,
                                        size_t maxBytes,
                                        const std::string& note) {
    if (s.size() <= maxBytes) return s;
    if (maxBytes <= note.size()) return note.substr(0, maxBytes);
    return utf8TruncateBytes(s, maxBytes - note.size()) + note;
}

std::string listFilesContent(const std::set<std::string>& testFiles, const std::string& workingDirectory, uint32_t maxInfoSize)
{
    std::string outputFilesContent;
    
    bool fitInContext = true;
    std::string fitInContextIssues;
    for(auto file : testFiles)
    {
        std::string fileName = boost_fs::path(file).filename().string();
        
        if(!boost_fs::exists(workingDirectory + "/" + file))
        {
            continue;
        }
        
        auto fileSize = boost_fs::file_size(workingDirectory + "/" + file);
        if(fileSize > maxInfoSize)
        {
            fitInContext = false;
            fitInContextIssues += "Size of the file: " + file + " it too big: " + std::to_string(fileSize) + "\n\n";
            continue;
        }
        
        const int maxCharacters = -1; //No limit here!!!
        
        std::string content = getFileContent(workingDirectory + "/" + file);

        outputFilesContent += "\n//File " + fileName + " starts here\n\n";
        outputFilesContent += content;
        outputFilesContent += "\n//File " + fileName + " ends here\n";
    }
    
    return outputFilesContent;
}


}
