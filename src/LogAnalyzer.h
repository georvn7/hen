#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Utils.h"

static constexpr uint32_t kInvUntracked = 0xFFFFFFFFu;

class LogAnalyzer {
public:
    struct SectionInfo {
        uint32_t line_start;   // 1-based, inclusive
        uint32_t line_end;     // 1-based, exclusive (same as your running end)
        bool     skipped;      // true => this is the “[[skip log lines]]” marker section
    };
    
    // Build index for the given log buffer (copies the buffer once into m_log).
    void parse(const std::string& log) {
        
        // Keep a single owned copy, then index via offsets.
        m_log = log;

        m_lines.clear();
        m_byFunc.clear();
        m_logLinesCount = 0;
        m_lineNoWidth   = 1;

        const char* base = m_log.data();
        const char* it   = base;
        const char* end  = base + m_log.size();

        // Heuristic reserve to cut reallocs (avg ~32 chars/line).
        m_lines.reserve(1u + m_log.size() / 32u);

        auto add_line = [&](const char* ls, const char* le) {
            const uint32_t start = static_cast<uint32_t>(ls - base);
            const uint32_t len   = static_cast<uint32_t>(le - ls);
            m_lines.push_back(Line{start, len});
            ++m_logLinesCount;

            // Parse header: optional ws, then name ":" invocation
            const char* p = ls;
            while (p < le && (*p == ' ' || *p == '\t')) ++p;

            // name begins at p, ends right before ':'
            const char* name_beg = p;
            while (p < le && *p != ':' && *p != ' ' && *p != '\t') ++p;
            if (p >= le || *p != ':') return; // no header → skip indexing this line
            const char* name_end = p;         // ':' at *p
            ++p;                               // move past ':'

            // Allow invocation to be either digits or "-1"
            bool neg = false;
            if (p < le && *p == '-') { neg = true; ++p; }

            if (p >= le || !std::isdigit(static_cast<unsigned char>(*p))) return;

            uint32_t invAbs = 0;
            while (p < le && std::isdigit(static_cast<unsigned char>(*p))) {
                invAbs = invAbs * 10u + static_cast<uint32_t>(*p - '0');
                ++p;
            }

            uint32_t inv = 0;
            if (neg) {
                if (invAbs != 1u) return;   // only support "-1"
                inv = kInvUntracked;           // sentinel for untracked
            } else {
                inv = invAbs;
            }

            // Insert posting: func → (invocation, line_idx)
            std::string func(name_beg, static_cast<size_t>(name_end - name_beg));
            auto& vec = m_byFunc[func];
            vec.push_back(Entry{inv, static_cast<uint32_t>(m_lines.size() - 1)});
        };

        while (it < end) {
            const void* nl = std::memchr(it, '\n', static_cast<size_t>(end - it));
            const char* le = nl ? static_cast<const char*>(nl) : end;
            add_line(it, le);
            it = nl ? (le + 1) : end;
        }

        // Sort per-function postings by (invocation, line_idx) for fast equal_range.
        for (auto& kv : m_byFunc) {
            auto& v = kv.second;
            std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) {
                return (a.invocation < b.invocation) ||
                       (a.invocation == b.invocation && a.line_idx < b.line_idx);
            });
        }

        // Cache width for printing 1-based line numbers like your original code.
        if (m_logLinesCount > 0) {
            uint32_t n = m_logLinesCount, w = 0;
            while (n) { n /= 10; ++w; }
            m_lineNoWidth = w ? w : 1;
        } else {
            m_lineNoWidth = 1;
        }
    }

    std::pair<std::string, size_t> logMessagesForFunction(
        const std::string& functionName,   // empty → match all
        std::size_t        startFromLine,  // 1-based
        std::size_t        invocation,     // 0 → match all
        std::size_t        charLimit) const
    {
        if (charLimit == 0) return {std::string(), 0};

        const auto totalLines = static_cast<uint32_t>(m_lines.size());
        const uint32_t startIdx = (startFromLine <= 1 ? 0u
                                 : (startFromLine > totalLines ? totalLines
                                                              : static_cast<uint32_t>(startFromLine - 1)));

        // Width for right-aligned line numbers (same as original).
        const int width = std::max<int>(1, static_cast<int>(std::to_string(m_logLinesCount).size()));

        // Output buffer heuristic reserve.
        std::string out;
        out.reserve(std::min<std::size_t>(charLimit + 256, charLimit + 4096));

        auto append_padded_line_no = [&](uint32_t oneBased) {
            std::string s = std::to_string(oneBased);
            if ((int)s.size() < width) out.append(static_cast<std::size_t>(width - static_cast<int>(s.size())), ' ');
            out += s;
            out.push_back(' ');
        };

        // Helpers to access a line slice and check "trim-left empty".
        auto line_view = [&](uint32_t li) -> std::pair<const char*, std::size_t> {
            const auto& L = m_lines[li];
            return { m_log.data() + L.start, static_cast<std::size_t>(L.len) }; // excludes '\n'
        };
        auto is_trim_left_empty = [&](const char* p, const char* e) -> bool {
            while (p < e && (*p == ' ' || *p == '\t')) ++p;
            return p >= e;
        };

        std::size_t written = 0;     // counts only raw+"\n" fully appended bytes (like original)
        uint32_t    streamLine = 0;  // final "to line:" value in footer
        bool        broke_on_partial = false;

        // Local worker that appends one specific line index (if possible).
        auto process_line = [&](uint32_t li) -> bool { // returns true to continue, false to stop
            // If we already hit/exceeded charLimit earlier, original code would break
            // on the next matching line encountered.
            if (written >= charLimit) {
                streamLine = li + 1; // 1-based
                return false;        // break
            }

            //const auto [ls, rawLen] = line_view(li);
            //const char* le = ls + rawLen;
            auto lv = line_view(li);
            const char* ls = lv.first;
            std::size_t rawLen = lv.second;
            const char* le = ls + rawLen;

            if (is_trim_left_empty(ls, le)) {
                return true; // skip empty/whitespace-only
            }

            const std::size_t need = rawLen + 1; // raw + '\n'
            if (written + need <= charLimit) {
                // Full append
                append_padded_line_no(li + 1);

                // Build (raw + '\n') into a temp, then filter (matches original behavior).
                std::string tmp;
                tmp.reserve(need);
                tmp.append(ls, rawLen);
                tmp.push_back('\n');

                out += hen::filterJsonText(tmp);
                written += need;
                return true;
            } else if (written < charLimit) {
                // Partial append: append only remaining bytes of (raw+'\n')
                const std::size_t remain = charLimit - written;

                append_padded_line_no(li + 1);

                std::string tmp;
                tmp.reserve(remain);
                // Copy up to 'remain' bytes from (raw+'\n')
                if (remain > 0) {
                    const std::size_t take = std::min<std::size_t>(remain, rawLen);
                    tmp.append(ls, take);
                    if (take < remain) tmp.push_back('\n'); // include '\n' if we still have room
                }
                out += hen::filterJsonText(tmp);

                // IMPORTANT: match original – do NOT increase 'written' here.
                // Break the loop.
                streamLine = li + 1;
                broke_on_partial = true;
                return false;
            } else {
                // written >= charLimit (handled at top), but keep symmetry:
                streamLine = li + 1;
                return false;
            }
        };
        
        auto wantedInv32 = [&]() -> uint32_t {
            // convention: size_t(-1) means "-1"
            if (invocation == static_cast<std::size_t>(-1)) return kInvUntracked;
            return static_cast<uint32_t>(invocation);
        }();

        auto line_has_invocation = [&](uint32_t li, uint32_t wanted) -> bool {
            auto lv = line_view(li);
            const char* p = lv.first;
            const char* e = p + lv.second;

            while (p < e && (*p == ' ' || *p == '\t')) ++p;

            // scan function token to ':'
            while (p < e && *p != ':' && *p != ' ' && *p != '\t') ++p;
            if (p >= e || *p != ':') return false;
            ++p;

            bool neg = false;
            if (p < e && *p == '-') { neg = true; ++p; }
            if (p >= e || !std::isdigit(static_cast<unsigned char>(*p))) return false;

            uint32_t invAbs = 0;
            while (p < e && std::isdigit(static_cast<unsigned char>(*p))) {
                invAbs = invAbs * 10u + static_cast<uint32_t>(*p - '0');
                ++p;
            }

            uint32_t inv = 0;
            if (neg) {
                if (invAbs != 1u) return false;
                inv = kInvUntracked;
            } else {
                inv = invAbs;
            }
            return inv == wanted;
        };

        // Iteration strategy:
        // - If functionName empty → scan all lines from startIdx.
        // - If functionName set and invocation != 0 → equal_range that invocation.
        // - If functionName set and invocation == 0 → iterate all entries for that function
        //   in ascending line order (stable with original scan behavior).

        if (functionName.empty()) {
            const bool filterInv = (invocation != 0);
            for (uint32_t i = startIdx; i < totalLines; ++i) {
                if (filterInv && !line_has_invocation(i, wantedInv32)) continue;
                if (!process_line(i)) break;
            }
            if (!broke_on_partial && streamLine == 0) streamLine = m_logLinesCount;
            
        } else {
            auto it = m_byFunc.find(functionName);
            if (it == m_byFunc.end()) {
                // No matches for this function — original would scan to EOF and print total line count.
                streamLine = m_logLinesCount;
            } else {
                const auto& vec = it->second;

                if (invocation != 0) {
                    // vec is sorted by (invocation, line_idx)
                    auto keyLo = LogAnalyzer::Entry{ wantedInv32, 0u };
                    auto keyHi = LogAnalyzer::Entry{ wantedInv32, UINT32_MAX };

                    auto lo = std::lower_bound(vec.begin(), vec.end(), keyLo,
                        [](const Entry& a, const Entry& b){
                            return (a.invocation < b.invocation) ||
                                   (a.invocation == b.invocation && a.line_idx < b.line_idx);
                        });
                    auto hi = std::upper_bound(vec.begin(), vec.end(), keyHi,
                        [](const Entry& a, const Entry& b){
                            return (a.invocation < b.invocation) ||
                                   (a.invocation == b.invocation && a.line_idx < b.line_idx);
                        });

                    // Walk only lines >= startFromLine, in ascending line_idx
                    for (auto jt = lo; jt != hi; ++jt) {
                        const uint32_t li = jt->line_idx;
                        const uint32_t oneBased = li + 1;
                        if (oneBased < startFromLine) continue;

                        if (!process_line(li)) {
                            // If we broke without partial and written == charLimit exactly
                            // BEFORE encountering this line, process_line would have set
                            // streamLine = oneBased; so nothing else to do.
                            break;
                        }
                    }

                    if (!broke_on_partial && streamLine == 0) {
                        // We didn't break in process_line. Two possibilities:
                        //  (a) We exhausted all matching entries before hitting the boundary:
                        //      original would keep scanning rest of file, so 'to line:' = total.
                        //  (b) We hit exact boundary right after fully appending the last match:
                        //      original would keep scanning non-matching lines until the next
                        //      *matching* line, and break there. If there's no next match, it hits EOF.
                        // Determine next matching line after all we processed:
                        //auto next = hi; // by default, none
                        // If we appended at least one from [lo,hi), try to find next matching after the last processed one:
                        // (We can’t know the “last processed” directly; however, if written >= charLimit we’d have broken.
                        //  So here written < charLimit → (a), or written == charLimit → choose next matching line.)
                        if (written == charLimit) {
                            // find the first entry in [hi, end) with line >= startFromLine (same invocation)
                            // but there IS no [hi,end) for same invocation; hi already points past all.
                            // Therefore: no next matching line → EOF.
                            streamLine = m_logLinesCount;
                        } else {
                            // (a) no boundary reached
                            streamLine = m_logLinesCount;
                        }
                    }
                } else {
                    // invocation == 0 → all invocations for this function, but we need overall line order.
                    std::vector<uint32_t> idxs;
                    idxs.reserve(vec.size());
                    for (const auto& e : vec) idxs.push_back(e.line_idx);
                    std::sort(idxs.begin(), idxs.end());

                    // Iterate in ascending line order, apply startFromLine filter.
                    uint32_t lastProcessed = 0;
                    bool     processedAny  = false;

                    for (std::size_t k = 0; k < idxs.size(); ++k) {
                        const uint32_t li = idxs[k];
                        const uint32_t oneBased = li + 1;
                        if (oneBased < startFromLine) continue;

                        if (!process_line(li)) {
                            // broke (either partial or boundary on next match)
                            break;
                        }
                        processedAny = true;
                        lastProcessed = oneBased;
                    }

                    if (!broke_on_partial && streamLine == 0) {
                        if (written == charLimit) {
                            // Exact boundary after a full append:
                            // Original would scan ahead to the *next matching* line and break there (or EOF if none).
                            // Find the first next matching line after the last processed.
                            bool foundNext = false;
                            if (processedAny) {
                                auto it2 = std::upper_bound(idxs.begin(), idxs.end(),
                                                            static_cast<uint32_t>(lastProcessed - 1));
                                if (it2 != idxs.end()) {
                                    streamLine = (*it2) + 1; // break would occur at this next matching line
                                    foundNext = true;
                                }
                            }
                            if (!foundNext) {
                                // No more matches → original would reach EOF
                                streamLine = m_logLinesCount;
                            }
                        } else {
                            // We just ran out of matches before hitting boundary.
                            streamLine = m_logLinesCount;
                        }
                    }
                }
            }
        }

        // Footer (identical text/ordering to your original).
        out.push_back('\n');
        out += "Parsed log file from line: ";
        out += std::to_string(startFromLine);
        out += " to line: ";
        out += std::to_string(streamLine);
        out.push_back('\n');
        out += "Total application log lines count: ";
        out += std::to_string(m_logLinesCount);
        out.push_back('\n');

        return { std::move(out), written };
    }
    
    std::pair<std::string, size_t>
    logMessagesForUntrackedFunctions(std::size_t startFromLine,
                                     std::size_t charLimit) const
    {
        return logMessagesForFunction(
            /*functionName*/ std::string(),          // all functions
            startFromLine,
            /*invocation*/ static_cast<std::size_t>(-1), // means "-1"
            charLimit
        );
    }

    std::pair<std::string, uint32_t> logGetLastInvocation(const std::string& function = std::string()) const
    {
        // Helper: check that the line matches your original "header … ' - ' …" expectation.
        auto has_dash_sep = [&](uint32_t li) -> bool {
            const Line& L = m_lines[li];
            std::string_view sv(m_log.data() + L.start, L.len); // excludes '\n'
            return sv.find(" - ") != std::string_view::npos;
        };

        // Case 1: specific function
        if (!function.empty()) {
            auto it = m_byFunc.find(function);
            if (it == m_byFunc.end()) {
                // No occurrences of this function
                return { std::string(), 0u };
            }

            const auto& vec = it->second;
            bool     any   = false;
            uint32_t bestLineIdx = 0;
            uint32_t bestInv     = 0;

            // We must return the LAST in log order, so pick the max line_idx
            for (const Entry& e : vec) {
                if (e.invocation == kInvUntracked) continue;
                if (!has_dash_sep(e.line_idx)) continue; // mimic old parser's " - " requirement
                if (!any || e.line_idx > bestLineIdx) {
                    any = true;
                    bestLineIdx = e.line_idx;
                    bestInv     = e.invocation;
                }
            }

            if (!any) return { std::string(), 0u };
            return { function, bestInv };
        }

        // Case 2: empty function → last header across ALL functions
        bool         any   = false;
        uint32_t     bestLineIdx = 0;
        uint32_t     bestInv     = 0;
        std::string  bestFunc;

        for (const auto& kv : m_byFunc) {
            const std::string& fname = kv.first;
            const auto&        vec   = kv.second;
            for (const Entry& e : vec) {
                if (e.invocation == kInvUntracked) continue;
                if (!has_dash_sep(e.line_idx)) continue;
                if (!any || e.line_idx > bestLineIdx) {
                    any = true;
                    bestLineIdx = e.line_idx;
                    bestInv     = e.invocation;
                    bestFunc    = fname;
                }
            }
        }

        if (!any) return { std::string(), 0u };
        return { bestFunc, bestInv };
    }
    
    uint32_t linesCount() const { return m_logLinesCount; }

    // Accessors you may find handy in your other methods:
    int lineNoWidth() const { return m_lineNoWidth; }
    std::string_view lineSlice(uint32_t lineIdx) const {
        const Line& L = m_lines[lineIdx];
        return std::string_view(m_log.data() + L.start, L.len);
    }
    
    bool empty() { return m_log.empty(); }
    
    size_t size() { return m_log.size(); }
    
    // Merge [start,end) intervals given as uint32_t.
    static std::vector<std::pair<uint32_t,uint32_t>>
    mergeIntervals(std::vector<std::pair<uint32_t,uint32_t>> ivals) {
        if (ivals.empty()) return ivals;
        std::sort(ivals.begin(), ivals.end(),
                  [](const std::pair<uint32_t,uint32_t>& a,
                     const std::pair<uint32_t,uint32_t>& b){
                      return (a.first < b.first) ||
                             (a.first == b.first && a.second < b.second);
                  });
        std::vector<std::pair<uint32_t,uint32_t>> out;
        out.reserve(ivals.size());
        std::pair<uint32_t,uint32_t> cur = ivals[0];
        for (size_t i = 1; i < ivals.size(); ++i) {
            if (ivals[i].first <= cur.second) {
                if (ivals[i].second > cur.second) cur.second = ivals[i].second;
            } else {
                out.push_back(cur);
                cur = ivals[i];
            }
        }
        out.push_back(cur);
        return out;
    }

    // Binary search membership: pos ∈ [start,end)
    static bool inIntervals(std::size_t pos,
                            const std::vector<std::pair<uint32_t,uint32_t>>& ivals) {
        if (ivals.empty()) return false;
        // Compare in 64-bit to avoid promotion pitfalls
        const uint64_t p = static_cast<uint64_t>(pos);
        size_t lo = 0, hi = ivals.size();
        while (lo < hi) {
            size_t mid = (lo + hi) >> 1;
            uint64_t s = ivals[mid].first;
            uint64_t e = ivals[mid].second; // half-open
            if (p < s) {
                hi = mid;
            } else if (p >= e) {
                lo = mid + 1;
            } else {
                return true;
            }
        }
        return false;
    }

    // Add an optional flag (default false) if you want to suppress skip callbacks.
    void forEachSectionByByteIntervals(
        const std::vector<std::pair<uint32_t, uint32_t>>& rawIntervals,
        std::size_t                                            sectionByteLimit,
        const std::function<void(const SectionInfo&, const std::string&)>& onSection,
        bool                                                   emitSkipMarkers // = false
    ) const {
        if (sectionByteLimit == 0 || m_lines.empty()) return;

        auto intervals = mergeIntervals(rawIntervals);

        std::size_t parsedSize = 0; // bytes scanned before current line (same as your loop)
        uint32_t    lineStart  = 1; // 1-based, inclusive
        uint32_t    lineEnd    = 1; // 1-based, inclusive for “current line”
        bool        inAnalyze  = false;
        std::string sectionBuf;
        sectionBuf.reserve(sectionByteLimit + 256);

        // NOTE: SectionInfo::line_end is INCLUSIVE here.
        // We pass the exact last line included in payload.
        const char* base = m_log.data();

        // Helper that emits a payload section and advances lineStart.
        // end_inclusive must be >= lineStart-1
        auto flush_payload_to = [&](uint32_t end_inclusive) {
            SectionInfo info;
            info.line_start = lineStart;
            info.line_end   = end_inclusive;
            info.skipped    = false;

            onSection(info, sectionBuf);
            sectionBuf.clear();

            // Next section starts on the line after the one we just included.
            lineStart = end_inclusive + 1;
        };

        for (uint32_t i = 0; i < static_cast<uint32_t>(m_lines.size()); ++i) {
            const Line& L = m_lines[i];
            const char* ls = base + L.start;
            std::size_t rawLen = static_cast<std::size_t>(L.len);

            bool analyse = inIntervals(parsedSize, intervals);
            parsedSize += rawLen + 1; // include '\n', same as your original

            if (analyse) {
                if (!inAnalyze) {
                    // Entering an analyze window: start a new section at current lineEnd.
                    lineStart = lineEnd;
                    inAnalyze = true;
                }

                // Append filtered line + '\n'
                static thread_local std::string tmp;
                tmp.clear();
                tmp.append(ls, rawLen);
                tmp.push_back('\n');
                sectionBuf += hen::filterJsonText(tmp);

                // If buffer reached the limit, flush including THIS line (lineEnd).
                if (sectionBuf.size() >= sectionByteLimit) {
                    flush_payload_to(lineEnd);
                }
            } else {
                if (inAnalyze) {
                    // Leaving analyze window: flush ONLY if we actually have payload.
                    if (!sectionBuf.empty()) {
                        // Current line is NOT included, so last included line is (lineEnd - 1)
                        flush_payload_to(lineEnd - 1);
                    }
                    if (emitSkipMarkers) {
                        SectionInfo skipInfo;
                        skipInfo.line_start = lineEnd;     // first non-analyzed line
                        skipInfo.line_end   = lineEnd;     // single-point marker is fine
                        skipInfo.skipped    = true;
                        onSection(skipInfo, std::string());
                    }
                }
                inAnalyze = false;
            }

            // Move to next line (lineEnd is kept as “current line” inclusive)
            ++lineEnd;
        }

        // Final flush for trailing payload (EOF while still analyzing)
        if (!sectionBuf.empty()) {
            flush_payload_to(lineEnd - 1);
        }
    }

    void clear()
    {
        m_lines.clear(); // all lines (order preserved)
        m_byFunc.clear();

        m_logLinesCount = 0;
        m_lineNoWidth   = 1;
        
        m_log.clear();
    }

private:
    struct Line {
        uint32_t start; // byte offset in m_log
        uint32_t len;   // bytes, excludes '\n'
    };
    struct Entry {
        uint32_t invocation;
        uint32_t line_idx; // index into m_lines
    };

    std::string m_log; // owned buffer

    std::vector<Line> m_lines; // all lines (order preserved)
    std::unordered_map<std::string, std::vector<Entry>> m_byFunc;

    uint32_t m_logLinesCount = 0;
    int      m_lineNoWidth   = 1;
};
