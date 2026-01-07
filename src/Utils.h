#pragma once

#include <iostream>
#include <string>
#include <stack>
#include <regex>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#elif __APPLE__
#include <sys/types.h>
#include <sys/sysctl.h>
#elif __linux__
#include <sys/ptrace.h>
#endif

#include <cpprest/json.h>
#include <iostream>
#include "IncludeBoost.h"

#define INVALID_HANDLE_ID 0xffffffff

#define NO_NODE_MESSAGES

namespace stdrave {

    std::string formatJson(const std::string& inputJson, const std::string& indentChar);
    bool validateJson(const web::json::value& jsonObject, const web::json::value& jsonSchema, std::string& log);

    inline std::string getPlatform() {
    #if defined(_WIN32) || defined(_WIN64)
        return "Windows";
    #elif defined(__APPLE__) || defined(__MACH__)
        return "macOS";
    #elif defined(__linux__)
        return "Linux";
    #elif defined(__unix__)
        return "Unix";
    #elif defined(_POSIX_VERSION)
        return "POSIX";
    #else
        return "Unknown";
    #endif
    }

    inline void killProcess(int pid) {
    #ifdef _WIN32
        // Open the process with termination rights.
        HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (hProcess == NULL) {
            return;
        }
        // Attempt to terminate the process.
        if (!TerminateProcess(hProcess, 1)) {
            CloseHandle(hProcess);
            return;
        }
        CloseHandle(hProcess);
    #else
        // Send SIGTERM to the process.
        kill(pid, SIGTERM);
    #endif
    }



    // Function to convert a string to lowercase
    inline std::string toLower(const std::string& str) {
        std::string lowerStr = str;
        std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(), ::tolower);
        return lowerStr;
    }

    inline bool compareCaseInsensitive(const std::string& str1, const std::string& str2)
    {
        return toLower(str1) == toLower(str2);
    }

    inline std::string trimLeadingWhitespace(const std::string &str) {
        size_t start = str.find_first_not_of(" \t\n\r\f\v");
        return (start == std::string::npos) ? "" : str.substr(start);
    }

    // Function to perform a case-insensitive search for "error"
    inline bool containsError(const std::string& text) {
        std::string lowerText = toLower(text);
        std::string lowerSearch = "error";
        return lowerText.find(lowerSearch) != std::string::npos;
    }

    inline bool isAFailure(const std::string& text) {
        std::string lowerText = toLower(text);
        std::string lowerSearch = "failure";
        return lowerText.find(lowerSearch) != std::string::npos;
    }

    inline std::string getLastAfter(const std::string& path, const std::string& splitStr) {
        std::size_t lastSlashPos = path.find_last_of(splitStr);
        if (lastSlashPos == std::string::npos) {
            return path; // No slash found, return the original path
        }
        return path.substr(lastSlashPos + 1);
    }

    //std::string findJson(const std::string& text);
    std::string getFirstSubdirectory(const boost_fs::path& dirPath);

    inline std::string loadFile(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return "";
        }

        std::stringstream buffer;
        buffer << file.rdbuf();  // Read the file buffer into the string stream
        file.close();            // Close the file handle
        return buffer.str();     // Convert the stringstream to std::string and return
    }

    inline void ensureEndsWith(std::string& str, char endingChar) {
        if (str.empty() || str.back() != endingChar) {
            str.push_back(endingChar);  // Append the character if it's not already the last character
        }
    }

    inline bool endsWith(const std::string& str, const std::string& suffix) {
        if (str.length() < suffix.length()) {
            return false;
        }
        return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
    }

    inline bool startsWith(const std::string& text, const std::string& prefix) {
        if (text.size() < prefix.size()) {
            return false;
        }
        return text.compare(0, prefix.size(), prefix) == 0;
    }

    inline void printAsComment(const std::string& input, std::ostream& output) {
        std::istringstream stream(input);
        std::string line;
        while (std::getline(stream, line)) {
            output << "// " << line << '\n';
        }
    }

    inline std::string parseAndFixLines(const std::string& input) {
        
        std::istringstream stream(input);
        std::string output;
        std::string line;
        while (std::getline(stream, line)) {
            output += (line + "\n");
        }
        
        return output;
    }

    inline size_t countLines(const std::string& str) {
        //return std::count(str.begin(), str.end(), '\n') + 1;
        
        //Maybe this is the correct version ?!?
        if (str.empty())
            return 0;
        
        auto newlines = std::count(str.begin(), str.end(), '\n');
        // If the very last character is NOT '\n', we have one more line
        return newlines + (str.back() != '\n' ? 1 : 0);
    }

    inline size_t countCStatements(const std::string& str) {
        return std::count(str.begin(), str.end(), ';') + 1;
    }

    // Helper struct to determine if the StringType is std::wstring
    template <typename T>
    struct is_wstring : std::false_type {};

    template <>
    struct is_wstring<std::wstring> : std::true_type {};

    // Function template to return narrow strings (std::string)
    template <typename StringType, typename std::enable_if<!is_wstring<StringType>::value, int>::type = 0>
    StringType _SL(const char* literal) {
        return StringType(literal);
    }

    // Function template to return wide strings (std::wstring)
    template <typename StringType, typename std::enable_if<is_wstring<StringType>::value, int>::type = 0>
    StringType _SL(const char* literal) {
        // Convert to std::wstring
        return std::wstring(literal, literal + std::char_traits<char>::length(literal));
    }

    template<typename StringType>
    typename StringType::size_type findTokensSequence(const StringType& mainStr, const StringType& first, const StringType& second) {
        // Define the appropriate regex type based on StringType
        using RegexType = std::basic_regex<typename StringType::value_type>;

        // Construct the regex pattern dynamically
        //StringType pattern = first + StringType(1, ' ') + StringType("(\\s*)") + second;
        StringType pattern = first + StringType(_SL<StringType>("(\\s*)")) + second;
        RegexType reg(pattern);

        std::match_results<typename StringType::const_iterator> match;
        if (std::regex_search(mainStr.begin(), mainStr.end(), match, reg)) {
            // Return the index of the first character after the match
            return match.position(0) + match.length(0);
        }

        return StringType::npos;  // Return -1 if no match is found
    }

    enum SourceExtraction
    {
        Normal,
        Truncated,
        Multiple,
        Actual,
        Skip,
        None
    };

    template<typename StringType>
    boost::optional<StringType> hasMarkdownSection(const StringType& text)
    {
        using RegexType = std::basic_regex<typename StringType::value_type>;
        //RegexType codeBlockPattern(R"(```(\w*)\s*[\s\S]*?```)");
        RegexType codeBlockPattern(StringType(_SL<StringType>("```(\\w*)\\s*[\\s\\S]*?```")));
        
        std::match_results<typename StringType::const_iterator> match;
        if (std::regex_search(text.begin(), text.end(), match, codeBlockPattern) && match.size() > 1) {
            return StringType(match[1].first, match[1].second);
        }
        
        return boost::none;
    }
    
    template<typename StringType>
    SourceExtraction findSourceSection(StringType& message,
                                       typename StringType::size_type& start_pos,
                                       typename StringType::size_type& end_pos,
                                       const StringType type)
    {
        using size_type = typename StringType::size_type;
        using Char      = typename StringType::value_type;

        auto ch = [](char c) -> Char { return static_cast<Char>(c); };

        if (type.empty()) {
            start_pos = end_pos = 0;
            return SourceExtraction::None;
        }

        // ASCII-only helpers
        auto is_space_or_tab = [&](Char c) {
            return c == ch(' ') || c == ch('\t');
        };
        auto ascii_tolower = [&](Char c) -> Char {
            if (c >= ch('A') && c <= ch('Z')) return static_cast<Char>(c - ch('A') + ch('a'));
            return c;
        };
        auto ascii_ieq = [&](Char a, Char b) { return ascii_tolower(a) == ascii_tolower(b); };

        const size_type N = message.size();
        size_type open_line_start = StringType::npos;
        size_type content_begin   = StringType::npos;
        size_type opening_ticks   = 0;

        // ── find opening fence ```<type> at start-of-line (spaces/tabs allowed) ─────────
        for (size_type i = 0; i < N; ) {
            // Skip indentation at line start
            size_type j = i;
            while (j < N && is_space_or_tab(message[j])) ++j;

            // Count backticks
            size_type k = j;
            while (k < N && message[k] == ch('`')) ++k;
            size_type ticks = k - j;

            if (ticks >= 3) {
                // Optional spaces after ticks
                size_type p = k;
                while (p < N && is_space_or_tab(message[p])) ++p;

                // Check language tag
                bool matches = true;
                if (p + type.size() <= N) {
                    for (size_type t = 0; t < type.size(); ++t) {
                        if (!ascii_ieq(message[p + t], type[t])) {
                            matches = false;
                            break;
                        }
                    }
                } else {
                    matches = false;
                }

                if (matches) {
                    // Move to end-of-line to begin fenced content
                    size_type eol = p + type.size();
                    while (eol < N && message[eol] != ch('\n') && message[eol] != ch('\r')) ++eol;
                    if (eol < N) {
                        if (message[eol] == ch('\r')) ++eol;
                        if (eol < N && message[eol] == ch('\n')) ++eol;
                    }
                    open_line_start = i;
                    content_begin   = eol;
                    opening_ticks   = ticks;
                    break;
                }
            }

            // Advance to next line
            while (i < N && message[i] != ch('\n') && message[i] != ch('\r')) ++i;
            while (i < N && (message[i] == ch('\n') || message[i] == ch('\r'))) ++i;
        }

        if (content_begin == StringType::npos) {
            start_pos = end_pos = 0;
            return SourceExtraction::None;
        }

        if (content_begin >= N) {
            start_pos = content_begin;
            end_pos   = N;
            return SourceExtraction::Truncated;
        }

        // ── scan for closing fence while respecting strings and char literals ──────────
        bool inString = false;
        bool inChar   = false;

        auto is_escaped_quote = [&](size_type pos) {
            // Count preceding backslashes to determine if quote at pos is escaped
            size_type k  = pos;
            size_type bs = 0;
            while (k > content_begin && message[k - 1] == ch('\\')) { --k; ++bs; }
            return (bs % 2) == 1;
        };

        for (size_type i = content_begin; i < N; ++i) {
            Char c = message[i];

            // Inside a C/C++ string literal
            if (inString) {
                if (c == ch('\\') && i + 1 < N) { ++i; continue; } // skip escaped payload
                if (c == ch('"') && !is_escaped_quote(i)) { inString = false; }
                continue;
            }

            // Inside a C/C++ char literal
            if (inChar) {
                if (c == ch('\\') && i + 1 < N) { ++i; continue; } // skip escaped payload
                if (c == ch('\'')) { inChar = false; }
                continue;
            }
            
            // --- NEW: skip C/C++ comments when not in quotes ------------------------
            if (c == ch('/')) {
                if (i + 1 < N && message[i + 1] == ch('/')) {
                    // single-line comment: skip to end of line, keep newline for at_line_start
                    i += 2;
                    while (i < N && message[i] != ch('\n') && message[i] != ch('\r')) ++i;
                    continue;
                }
                if (i + 1 < N && message[i + 1] == ch('*')) {
                    // multi-line comment: skip to closing */
                    i += 2;
                    while (i + 1 < N && !(message[i] == ch('*') && message[i + 1] == ch('/'))) ++i;
                    if (i + 1 < N) i += 2; // step past */
                    continue;
                }
            }
            // -----------------------------------------------------------------------

            // Not inside any quoted context yet: enter if we see openers
            if (c == ch('"'))  { inString = true; continue; }
            if (c == ch('\'')) { inChar   = true; continue; }

            // Only consider a closing fence at true line start
            bool at_line_start = (i == content_begin) ||
                                 (message[i - 1] == ch('\n') || message[i - 1] == ch('\r'));

            if (at_line_start) {
                size_type j = i;
                while (j < N && is_space_or_tab(message[j])) ++j;

                size_type k = j;
                while (k < N && message[k] == ch('`')) ++k;
                size_type ticks = k - j;

                if (ticks >= 3 && ticks >= opening_ticks) {
                    start_pos = content_begin;
                    end_pos   = i;
                    return SourceExtraction::Normal;
                }
            }
        }

        // No closing fence found
        start_pos = content_begin;
        end_pos   = N;
        return SourceExtraction::Truncated;
    }

    template<typename CharType>
    using BasicString = std::basic_string<CharType>;

    //Some LLMs spam repetative lines. Very long text sequences to consime the full context window.
    template<typename CharType>
    BasicString<CharType> filterRepetativeLines(const BasicString<CharType>& input)
    {
        BasicString<CharType> output;
        std::set<BasicString<CharType>> uniqueLines;
        
        std::basic_istringstream<CharType> iss(input);
        
        BasicString<CharType> line;
        while (std::getline(iss, line))
        {
            if(uniqueLines.find(line) == uniqueLines.end())
            {
                output += line + "\n";
            }
            uniqueLines.insert(line);
        }
        
        return output;
    }

    template <typename StringType>
    SourceExtraction findSource(const StringType& message, StringType& source, const StringType type) {
        if (type.empty()) { source = filterRepetativeLines(message); return SourceExtraction::None; }

        StringType tmp = message;
        typename StringType::size_type s=0,e=0;
        SourceExtraction res = findSourceSection(tmp, s, e, type);
        if (res == SourceExtraction::Normal || res == SourceExtraction::Truncated) {
            source = tmp.substr(s, e - s);   // one block only
            return res;
        }
        source.clear();
        return SourceExtraction::None;
    }

    //THIS FUNCTION IS BUGGU !!!
    template<typename CharType>
    BasicString<CharType> fixJsonQuotes(const BasicString<CharType>& input) {
        std::basic_stringstream<CharType> ss(input);
        BasicString<CharType> output, line;
        
        auto addQuotes = [](BasicString<CharType>& str) {
            for (auto& ch : str) {
                if (ch == '\'' || ch == '`') {
                    ch = '\"';
                }
            }
            if (str.front() != '\"') {
                str.insert(str.begin(), '\"');
            }
            if (str.back() != '\"') {
                str.push_back('\"');
            }
        };

        while (getline(ss, line)) {
            BasicString<CharType> commentStr = _SL<BasicString<CharType>>("//");
            // Simple check to skip comment lines
            if (line.find(commentStr) == BasicString<CharType>::npos) {
                typename BasicString<CharType>::size_type pos = 0;
                // Add double quotes around keys
                while ((pos = line.find(':', pos)) != BasicString<CharType>::npos) {
                    typename BasicString<CharType>::size_type start = line.rfind(' ', pos);
                    BasicString<CharType> key = line.substr(start + 1, pos - start - 1);
                    addQuotes(key);
                    line.replace(start + 1, pos - start - 1, key);
                    pos += 1;
                }
                output += line;
                output += '\n';
            }
        }
        
        return output;
    }

    template<typename CharType>
    BasicString<CharType> fixJsonCommas(const BasicString<CharType>& input) {
        BasicString<CharType> output = input;

        // Helper lambda to replace all occurrences of a substring
        auto replaceAll = [](BasicString<CharType>& str, const BasicString<CharType>& from, const BasicString<CharType>& to) {
            size_t startPos = 0;
            while((startPos = str.find(from, startPos)) != BasicString<CharType>::npos) {
                str.replace(startPos, from.length(), to);
                startPos += to.length(); // Handles case to avoid infinite loop
            }
        };

        // Replace unneeded commas
        replaceAll(output, BasicString<CharType>({',', '}'}), BasicString<CharType>({'}'}));
        replaceAll(output, BasicString<CharType>({',', '\n', '}'}), BasicString<CharType>({'}'}));

        replaceAll(output, BasicString<CharType>({',', ']'}), BasicString<CharType>({']'}));
        replaceAll(output, BasicString<CharType>({',', '\n', ']'}), BasicString<CharType>({']'}));

        return output;
    }


    template<typename CharType>
    BasicString<CharType> fixJsonNewlines(const BasicString<CharType>& input) {
        BasicString<CharType> out;
        out.reserve(input.size());
        std::stack<CharType> brackets;
        bool inString = false;

        auto ch = [](char c){ return static_cast<CharType>(c); };
        auto isEscapedQuote = [&](size_t i){
            size_t k = i, bs = 0;
            while (k > 0 && input[k-1] == ch('\\')) { --k; ++bs; }
            return (bs % 2) == 1; // odd => escaped
        };

        for (size_t i = 0; i < input.size(); ++i) {
            CharType c = input[i];
            if (!inString) {
                if (c == ch('"')) {
                    if (!isEscapedQuote(i)) inString = true;
                    out.push_back(c);
                } else {
                    if (c == ch('{') || c == ch('[')) brackets.push(c);
                    else if (c == ch('}') && !brackets.empty() && brackets.top() == ch('{')) brackets.pop();
                    else if (c == ch(']') && !brackets.empty() && brackets.top() == ch('[')) brackets.pop();
                    out.push_back(c);
                }
            } else {
                if (c == ch('"') && !isEscapedQuote(i)) {
                    inString = false;
                    out.push_back(c);
                } else if (c == ch('\n')) {
                    out.push_back(ch('\\'));
                    out.push_back(ch('n'));
                } else if (c == ch('\r')) {
                    // drop CR in strings
                } else {
                    out.push_back(c);
                }
            }
        }
        return out;
    }

    template<typename CharType>
    BasicString<CharType> trimJson(const BasicString<CharType>& input) {
        const BasicString<CharType> whitespace = BasicString<CharType>(1, static_cast<CharType>(' ')) +
                                                 BasicString<CharType>(1, static_cast<CharType>('\n')) +
                                                 BasicString<CharType>(1, static_cast<CharType>('\r')) +
                                                 BasicString<CharType>(1, static_cast<CharType>('\t'));
        
        size_t start = input.find_first_not_of(whitespace);
        if (start == BasicString<CharType>::npos) return BasicString<CharType>();

        size_t end = input.find_last_not_of(whitespace);
        return input.substr(start, end - start + 1);
    }

    template<typename CharType>
    BasicString<CharType> unescapeJson(const BasicString<CharType>& input) {
        BasicString<CharType> output = input;
        
        // Replace \\n with actual newlines
        std::basic_regex<CharType> escNewline(
            BasicString<CharType>(1, static_cast<CharType>('\\')) +
            BasicString<CharType>(1, static_cast<CharType>('\\')) +
            BasicString<CharType>(1, static_cast<CharType>('n'))
        );
        output = std::regex_replace(output, escNewline, BasicString<CharType>(1, static_cast<CharType>('\n')));
        
        // Replace \\" with "
        std::basic_regex<CharType> escQuote(
            BasicString<CharType>(1, static_cast<CharType>('\\')) +
            BasicString<CharType>(1, static_cast<CharType>('\\')) +
            BasicString<CharType>(1, static_cast<CharType>('"'))
        );
        output = std::regex_replace(output, escQuote, BasicString<CharType>(1, static_cast<CharType>('"')));
        
        // Add more replacements here if needed
        
        return output;
    }

    template<typename CharType>
    bool canParseAsJson(const BasicString<CharType>& candidate) {
        try {
            auto parsed = web::json::value::parse(candidate);
            // If we reach here, parsing succeeded
            return true;
        }
        catch (const web::json::json_exception&) {
            // or whatever exception your library throws
            return false;
        }
    }

    // In-place BOM removers (C++14)
    template<typename CharT>
    inline typename std::enable_if<sizeof(CharT) == 1, void>::type
    strip_bom_in_place(std::basic_string<CharT>& s) {
        // UTF-8 BOM: EF BB BF
        if (s.size() >= 3 &&
            static_cast<unsigned char>(s[0]) == 0xEF &&
            static_cast<unsigned char>(s[1]) == 0xBB &&
            static_cast<unsigned char>(s[2]) == 0xBF) {
            s.erase(0, 3);
        }
    }

    template<typename CharT>
    inline typename std::enable_if<sizeof(CharT) != 1, void>::type
    strip_bom_in_place(std::basic_string<CharT>& s) {
        // Leading U+FEFF (BOM / ZERO WIDTH NO-BREAK SPACE)
        if (!s.empty() && s[0] == static_cast<CharT>(0xFEFF)) {
            s.erase(0, 1);
        }
    }

    // Optional helper if you prefer a copy
    template<typename CharT>
    inline std::basic_string<CharT> strip_bom(std::basic_string<CharT> s) {
        strip_bom_in_place(s);
        return s;
    }

    template<typename CharType>
    std::basic_string<CharType>
    fixJsonStringEscapes(const std::basic_string<CharType>& in)
    {
        using S = std::basic_string<CharType>;
        auto ch = [](char c){ return static_cast<CharType>(c); };
        auto isHex = [&](CharType c){
            return (c >= ch('0') && c <= ch('9')) ||
                   (c >= ch('A') && c <= ch('F')) ||
                   (c >= ch('a') && c <= ch('f'));
        };

        S out;
        out.reserve(in.size() + in.size()/16);

        bool inString = false;

        for (size_t i = 0; i < in.size(); ++i) {
            CharType c = in[i];

            if (c == ch('"')) {
                // odd-backslash rule: only toggle if not escaped
                size_t k = i, bs = 0;
                while (k > 0 && in[k-1] == ch('\\')) { --k; ++bs; }
                if ((bs % 2) == 0) inString = !inString;
                out.push_back(c);
                continue;
            }

            if (!inString) { out.push_back(c); continue; }

            // Inside a string
            if (c == ch('\\')) {
                if (i + 1 >= in.size()) {
                    // trailing backslash -> escape it
                    out.push_back(ch('\\')); out.push_back(ch('\\'));
                    continue;
                }
                CharType nxt = in[i + 1];

                // valid \uXXXX ?
                if (nxt == ch('u')) {
                    if (i + 5 < in.size() &&
                        isHex(in[i+2]) && isHex(in[i+3]) &&
                        isHex(in[i+4]) && isHex(in[i+5])) {
                        out.push_back(ch('\\')); out.push_back(ch('u'));
                        out.push_back(in[i+2]); out.push_back(in[i+3]);
                        out.push_back(in[i+4]); out.push_back(in[i+5]);
                        i += 5; // consumed \uXXXX
                        continue;
                    } else {
                        // make it literal: \\u...
                        out.push_back(ch('\\')); out.push_back(ch('\\')); out.push_back(ch('u'));
                        ++i; // consumed 'u'
                        continue;
                    }
                }

                // allowed simple escapes
                if (nxt==ch('"')||nxt==ch('\\')||nxt==ch('/')||
                    nxt==ch('b')||nxt==ch('f')||nxt==ch('n')||nxt==ch('r')||nxt==ch('t')) {
                    out.push_back(ch('\\')); out.push_back(nxt);
                    ++i;
                    continue;
                }

                // invalid escape: \x -> \\x   (fixes \0, \e, etc.)
                out.push_back(ch('\\')); out.push_back(ch('\\')); out.push_back(nxt);
                ++i;
                continue;
            }

            // Control characters must be escaped in JSON strings
            unsigned int uc = static_cast<unsigned int>(c);
            if (uc <= 0x1F) {
                switch (uc) {
                    case '\b': out.push_back(ch('\\')); out.push_back(ch('b')); break;
                    case '\f': out.push_back(ch('\\')); out.push_back(ch('f')); break;
                    case '\n': out.push_back(ch('\\')); out.push_back(ch('n')); break;
                    case '\r': out.push_back(ch('\\')); out.push_back(ch('r')); break;
                    case '\t': out.push_back(ch('\\')); out.push_back(ch('t')); break;
                    default: {
                        out.push_back(ch('\\')); out.push_back(ch('u'));
                        char hex[5];
                        std::snprintf(hex, sizeof(hex), "%04X", uc & 0xFF);
                        out.push_back(ch(hex[0])); out.push_back(ch(hex[1]));
                        out.push_back(ch(hex[2])); out.push_back(ch(hex[3]));
                    } break;
                }
                continue;
            }

            out.push_back(c); // normal char
        }

        return out;
    }

    template<typename CharType>
    std::basic_string<CharType>
    findJson(const std::basic_string<CharType>& content, bool fix)
    {
        using S = std::basic_string<CharType>;
        auto ch = [](char c){ return static_cast<CharType>(c); };

        // 1) Try fenced ```json blocks first (your existing helper)
        S text;
        SourceExtraction sr = findSource(content, text, S{ch('j'),ch('s'),ch('o'),ch('n')});
        strip_bom_in_place(text);
        auto can = [&](const S& s){ return canParseAsJson(s); };

        auto cutToBraces = [&](const S& s)->S {
            auto o = s.find(ch('{'));
            auto c = s.rfind(ch('}'));
            return (o!=S::npos && c!=S::npos && o<c) ? s.substr(o, c-o+1) : S{};
        };

        if (!text.empty()) {
            S inner = cutToBraces(text);
            if (!inner.empty()) {
                if (can(inner)) return inner;
                if (fix) {
                    S innerFixed = fixJsonStringEscapes<CharType>(inner);
                    if (can(innerFixed)) return innerFixed;
                    inner = std::move(innerFixed); // (optional) keep improved version in case of later fixes
                }
            }
        }

        // 2) General fallback: scan all candidates and pick the first that parses
        const S& all = content;
        int n = static_cast<int>(all.size());

        auto isEscapedQuote = [&](int i){
            int k = i - 1, bs = 0;
            while (k >= 0 && all[(size_t)k] == ch('\\')) { --k; ++bs; }
            return (bs % 2) == 1; // odd => escaped
        };

        for (int start = 0; start < n; ++start) {
            if (all[(size_t)start] != ch('{')) continue;

            bool inString = false;
            int depth = 0;
            for (int i = start; i < n; ++i) {
                CharType c = all[(size_t)i];

                if (c == ch('"')) {
                    if (!isEscapedQuote(i)) inString = !inString;
                } else if (!inString) {
                    if (c == ch('{')) ++depth;
                    else if (c == ch('}')) {
                        if (--depth == 0) {
                            // Candidate slice [start..i]
                            S cand = all.substr((size_t)start, (size_t)(i - start + 1));
                            strip_bom_in_place(cand);

                            // Optional: fix illegal escapes only inside strings
                            if (fix && !can(cand)) {
                                cand = fixJsonStringEscapes<CharType>(cand);
                            }
                            if (can(cand)) return cand;
                            break; // this start failed; try next '{'
                        }
                    }
                }
            }
        }

        // 3) Nothing parsed; last resort: return empty
        return S{};
    }

    // Template function to remove the <think> part from both std::string and std::wstring
    template <typename StringType>
    StringType removeThinkPart(const StringType& input) {
        // Define the <think> and </think> tags based on the string type
        StringType thinkStartTag = StringType("<think>");
        StringType thinkEndTag = StringType("</think>");

        // Find the positions of the tags
        std::size_t thinkStart = input.find(thinkStartTag);
        std::size_t thinkEnd = input.find(thinkEndTag);

        // If both tags are found, remove the <think> part
        if (thinkStart != StringType::npos && thinkEnd != StringType::npos) {
            // Calculate the length of the <think> part including the tags
            std::size_t thinkLength = thinkEnd - thinkStart + thinkEndTag.length();

            // Remove the <think> part from the string
            StringType result = input;
            result.erase(thinkStart, thinkLength);
            return result;
        }

        // If the tags are not found, return the original string
        return input;
    }

    enum class IncludeType {
        AngleBracket,
        DoubleQuote,
        Invalid
    };

    // Utility function to escape all regex special characters in a string
    inline std::string escapeRegex(const std::string& s) {
        static const std::regex specialChars{R"([-[\]{}()*+?.,\^$|#\s])"};
        return std::regex_replace(s, specialChars, R"(\$&)");
    }

    template<typename T>
    bool contains(std::stack<T> s, const T& element) {
        while (!s.empty()) {
            if (s.top() == element) {
                return true; // Element found
            }
            s.pop(); // Remove the top element
        }
        return false; // Element not found
    }

    template <typename T>
    std::pair<std::set<T>, std::set<T>> getSetDifferences(const std::set<T>& setA, const std::set<T>& setB)
    {
        std::set<T> newEntries;    // Elements in B but not in A
        std::set<T> removedEntries;// Elements in A but not in B
        
        // Find new entries (B - A)
        std::set_difference(
            setB.begin(), setB.end(),
            setA.begin(), setA.end(),
            std::inserter(newEntries, newEntries.begin())
        );
        
        // Find removed entries (A - B)
        std::set_difference(
            setA.begin(), setA.end(),
            setB.begin(), setB.end(),
            std::inserter(removedEntries, removedEntries.begin())
        );
        
        return {newEntries, removedEntries};
    }

    // Templated function to accept any callable (like a lambda) that computes the number.
    template <typename NumberFunc>
    std::vector<std::string> sortSetByNumber(const std::set<std::string>& inputSet, NumberFunc getNumber) {
        
        std::vector<std::string> sortedVector(inputSet.begin(), inputSet.end());
        
        std::sort(sortedVector.begin(), sortedVector.end(),
                  [&getNumber](const std::string &a, const std::string &b) {
                      return getNumber(a) < getNumber(b);
                  });
        
        return sortedVector;
    }

    // Templated function to accept any callable (like a lambda) that computes the number.
    template <typename NumberFunc>
    std::vector<std::shared_ptr<std::string>> sortSetByNumber2(const std::set<std::string>& inputSet, NumberFunc getNumber) {
        // Create a vector of shared pointers, copying each element from the set.
        std::vector<std::shared_ptr<std::string>> result;
        result.reserve(inputSet.size());
        for (const auto& entry : inputSet) {
            result.push_back(std::make_shared<std::string>(entry));
        }
        
        // Sort the vector. Dereference the shared_ptrs to pass strings to getNumber.
        std::sort(result.begin(), result.end(),
                  [&getNumber](const std::shared_ptr<std::string>& a, const std::shared_ptr<std::string>& b) {
                      return getNumber(*a) < getNumber(*b);
                  });
        
        return result;
    }

    inline std::string replaceAll(std::string subject, const std::string& search, const std::string& replace) {
        size_t pos = 0;
        while ((pos = subject.find(search, pos)) != std::string::npos) {
            subject.replace(pos, search.length(), replace);
            pos += replace.length(); // Advance past the replaced portion.
        }
        return subject;
    }

    // Shell-quote a string using single quotes (safe for sh/zsh).
    inline std::string shQuote(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('\'');
        for (char c : s) {
            if (c == '\'') out += "'\\''";  // close, escape, reopen
            else out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    IncludeType getIncludeType(const std::string& includeStatement);
    std::string getLastToken(const std::string& str, char delimiter);
    std::string jsonSchemaToCStruct(web::json::value& schema);

    std::string extractDataType(const std::string& type);
    std::string removeWhitespace(const std::string &str);
    std::string extractType(const std::string& declaration, const std::string& name);
    std::string removeComments(const std::string &input, std::string &removed_out);
    std::string getSourceType(const std::string& fileExtension);
    bool hasMainFunction(const std::string& objFilePath);
    std::string checkStringEnd(const std::string& input);
    std::string exec(const std::string& cmd, const std::string& workingDir, const std::string& operation, bool deleteOutput);

    struct ExecResult {
        std::string output;
        int exit_code;     // shell exit code (or -1 if timed out)
        bool timed_out;
    };
    ExecResult exec_with_timeout(const std::string& cmd,
                                 const std::string& workingDir,
                                 const std::string& operation,
                                 bool deleteOutput,
                                 std::chrono::milliseconds timeout);

    std::unique_ptr<boost_prc::process> spawn(const std::string& cmd, const std::string& workingDir,
                                         const std::string& outputDir, const std::string& operation,
                                              bool detached);
    bool isDebuggerAttached();
    void waitForDebugger();
    std::string getLANIP();
    web::json::value findInJson(const web::json::value& object, const std::string& propertyPath);
    bool startsWithIgnoreCase(const std::string &s1, const std::string &s2);
    void setupEnv();
    std::string buildPrompt(const std::string& content, const std::map<std::string, std::string>& params);
    bool addToSet(std::vector<std::shared_ptr<std::string>>& vset, const std::string& element);
    bool isInSet(std::vector<std::shared_ptr<std::string>>& vset, const std::string& element);
    std::string getAsCsv(const std::set<std::string>& namesSet);
    std::vector<std::shared_ptr<std::string>> readFromCsv(const std::string& csv);
    std::string getAsList(const std::vector<std::shared_ptr<std::string>>& namesSet);
    uint32_t generateUniqueUint32();
    std::string generateUUID();
    std::string generateApiKey();
    std::string getCurrentDateTime();
    std::string getClangResourceDir();
    std::string getClangInclude();
    std::string getCppInclude();
    std::string getSysRoot();
    std::string getSDKVersion();
    std::string getExePath();
    std::string replaceDisallowedChars(const std::string& input);
    std::string filterLinesByPattern(const std::string& text, const std::string& pattern);
    std::pair<std::string, std::string> splitByFirstOccurence(const std::string& str, char delimiter);
    std::string makeCanonical(const std::string& path);
    std::string getTargetFromUrl(const std::string &urlString);

    std::string zipDirectory(const std::string& dirPath,
                             const std::string& customZipName,
                             const std::string& password);
    
    std::ifstream openFile(const std::string& fileName, const std::set<std::string>& searchPaths = {});
    std::string getFileContent(const std::string& fileName, const std::set<std::string>& searchPaths = {});
    boost::optional<std::string> getFirstLine(const std::string& fileName, const std::set<std::string>& searchPaths = {});
    bool loadJson(web::json::value& json, const std::string& path);
    bool saveJson(const web::json::value& json, const std::string& path);
    bool saveToFile(const std::string& content, const std::string& path);

    std::string filterJsonText(const std::string& input);
    bool isBinaryFile(const std::string& filePath);
    std::string readTextFileWithLimit(const std::string &filePath, std::size_t maxBytes);
    std::string printLineNumbers(const std::string& text, int lineOffset);
    std::string normalizeNewLines(const std::string& input);
    int mapLine(const std::string& from, const std::string& to, int lineNumber);
    int normalizeAndMapLine(const std::string& from, const std::string& to, int lineNumber);
    std::pair<std::string, int> insertSnippet(const std::string& text,  // The original text
                                          const std::string& snippet,   // The snippet to insert
                                          int lineNumber,               // 1-based line number at which to insert
                                          bool skipWhitespaces);        // Skip leading whitespaces on that line

    std::pair<std::string, bool> insertStringAt(const std::string& A, const std::string& B,
                                                uint32_t line, uint32_t col);

    std::size_t getFileHash(const std::string& filePath);
    std::vector<std::string> parseCommandLine(const std::string &cmdLine);
    bool isTextOnlyFile(const std::string& path);
    bool isTextFileAsciiOrUtf8(const std::string& path);
    bool parsePrefixFlags(const std::string& s,
                          bool& debug,
                          bool& checkResult,
                          std::string& expectedResult,
                          std::string& stdOutRegex,
                          std::string& stripped);

    bool tryMakeRegex(const std::string& pattern, std::regex& out,
                            std::regex_constants::syntax_option_type flags,
                            std::string* error) noexcept;

    std::pair<std::string, int32_t> makeStringNumberPair(const std::string& pairStr);
    std::string trim(const std::string& str);
    boost_fs::path findDirectoryByName(const boost_fs::path& root, const std::string& dirName);

    template <typename T>
    bool objFromJson(const std::string& path, T* object)
    {
        web::json::value json;
        if(!loadJson(json, path))
        {
            return false;
        }
        
        object->from_json(json);
        return true;
    }

    bool fullRegexMatch(const std::string& logRaw,
                            const std::string& pattern,
                           std::string& err);

    void alternateRoles(web::json::value& body);
    std::set<std::string> extractFilesFromCommandLine(const std::string& commandLine);

    std::size_t nextIndex(const boost_fs::path& dir, const std::string& prefix);
}
