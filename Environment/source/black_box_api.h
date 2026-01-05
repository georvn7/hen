#pragma once


namespace black_box_api {

enum : uint16_t {
    MSG_TRACE_APPEND    = 1,   // payload = bytes to append to trace.txt
    MSG_STDOUT_APPEND   = 2,   // payload = bytes to append to stdout.log
    MSG_MEMO_REPLACE    = 3,   // payload = full memo.txt text (atomic replace)
    MSG_STACK_REOPEN    = 4,   // payload = uint8_t truncate(0/1) + path bytes (no NUL)
    MSG_STACK_REMOVE    = 5,   // payload = empty
    MSG_STACK_WRITE     = 6,   // payload = bytes to append to current stack file
    MSG_FLUSH_SYNC      = 7,   // payload = empty (helper fdatasync)
    MSG_SAVE_BREAKPOINT = 8,
    MSG_TRACE_AND_STACK = 9,
    MSG_FINISH_STOP     = 0xFF // payload = empty (helper flush+exit)
};

#pragma pack(push,1)
struct MsgHdr {
    uint32_t len;    // payload length in bytes
    uint16_t type;   // one of the codes above
    uint16_t flags;  // reserved = 0
};
#pragma pack(pop)

} // namespace black_box
