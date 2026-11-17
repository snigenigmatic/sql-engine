#pragma once

#include "execution/executor.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sql::net
{

    // Client/server wire protocol. Every message is one frame:
    //
    //   u32 length (big-endian) | u8 type | payload (length - 1 bytes)
    //
    // Integers in payloads are big-endian; strings are a u32 length then
    // the bytes. Rows use Tuple::SerializeTo (host byte order: a cluster's
    // machines are assumed to share an architecture).
    enum class MessageType : uint8_t
    {
        // client → server
        EXECUTE = 1, // u8 mode (ExecuteMode), string sql
        COMMAND = 2, // string name, string argument
        CALL = 6,    // string method, u32 count, then each argument: an
                     // internal request between cluster processes
        // server → client
        RESULTS = 3, // u8 in_transaction, u32 count, then each result
        TEXT = 4,    // u8 in_transaction, string text
        ERROR = 5,   // string message: the request itself failed
    };

    enum class ExecuteMode : uint8_t
    {
        STATEMENT = 0,            // exactly one statement (Session::Execute)
        SCRIPT = 1,               // every statement (Session::ExecuteScript)
        SCRIPT_STOP_ON_ERROR = 2, // ... stopping at the first failure
    };

    // Larger frames are refused before anything is allocated for them
    constexpr uint32_t MAX_FRAME_SIZE = 256u << 20;

    struct Frame
    {
        MessageType type = MessageType::ERROR;
        std::string payload;
    };

    // The frame's bytes on the wire, header included
    std::string EncodeFrame(const Frame &frame);

    // Builds a payload
    class Writer
    {
    public:
        void U8(uint8_t value) { out_.push_back(static_cast<char>(value)); }
        void U32(uint32_t value);
        void String(const std::string &value);
        std::string Take() { return std::move(out_); }

    private:
        std::string out_;
    };

    // Reads a payload; every read fails (returns false) past the end
    class Reader
    {
    public:
        explicit Reader(const std::string &data) : data_(data) {}
        bool U8(uint8_t *value);
        bool U32(uint32_t *value);
        bool String(std::string *value);
        bool AtEnd() const { return pos_ == data_.size(); }

    private:
        const std::string &data_;
        size_t pos_ = 0;
    };

    struct ExecuteRequest
    {
        ExecuteMode mode = ExecuteMode::STATEMENT;
        std::string sql;
    };

    struct CommandRequest
    {
        std::string name;
        std::string argument;
    };

    struct CallRequest
    {
        std::string method;
        std::vector<std::string> arguments;
    };

    struct ResultsResponse
    {
        bool in_transaction = false;
        std::vector<ExecutionResult> results;
    };

    struct TextResponse
    {
        bool in_transaction = false;
        std::string text;
    };

    Frame Encode(const ExecuteRequest &request);
    Frame Encode(const CommandRequest &request);
    Frame Encode(const CallRequest &request);
    Frame Encode(const ResultsResponse &response);
    Frame Encode(const TextResponse &response);
    Frame EncodeError(const std::string &message);

    // Each decodes a frame of its own type; nullopt if the frame is of
    // another type or malformed (truncated, trailing bytes, bad row)
    std::optional<ExecuteRequest> DecodeExecute(const Frame &frame);
    std::optional<CommandRequest> DecodeCommand(const Frame &frame);
    std::optional<CallRequest> DecodeCall(const Frame &frame);
    std::optional<ResultsResponse> DecodeResults(const Frame &frame);
    std::optional<TextResponse> DecodeText(const Frame &frame);
    std::optional<std::string> DecodeError(const Frame &frame);

} // namespace sql::net
