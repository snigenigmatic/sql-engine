#include "net/protocol.h"

namespace sql::net
{

    std::string EncodeFrame(const Frame &frame)
    {
        Writer header;
        header.U32(static_cast<uint32_t>(frame.payload.size() + 1));
        header.U8(static_cast<uint8_t>(frame.type));
        return header.Take() + frame.payload;
    }

    void Writer::U32(uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
            out_.push_back(static_cast<char>((value >> shift) & 0xFF));
    }

    void Writer::String(const std::string &value)
    {
        U32(static_cast<uint32_t>(value.size()));
        out_ += value;
    }

    bool Reader::U8(uint8_t *value)
    {
        if (data_.size() - pos_ < 1)
            return false;
        *value = static_cast<uint8_t>(data_[pos_++]);
        return true;
    }

    bool Reader::U32(uint32_t *value)
    {
        if (data_.size() - pos_ < 4)
            return false;
        uint32_t result = 0;
        for (int i = 0; i < 4; ++i)
            result = (result << 8) | static_cast<uint8_t>(data_[pos_++]);
        *value = result;
        return true;
    }

    bool Reader::String(std::string *value)
    {
        uint32_t size = 0;
        if (!U32(&size) || data_.size() - pos_ < size)
            return false;
        value->assign(data_, pos_, size);
        pos_ += size;
        return true;
    }

    Frame Encode(const ExecuteRequest &request)
    {
        Writer w;
        w.U8(static_cast<uint8_t>(request.mode));
        w.String(request.sql);
        return {MessageType::EXECUTE, w.Take()};
    }

    Frame Encode(const CommandRequest &request)
    {
        Writer w;
        w.String(request.name);
        w.String(request.argument);
        return {MessageType::COMMAND, w.Take()};
    }

    Frame Encode(const CallRequest &request)
    {
        Writer w;
        w.String(request.method);
        w.U32(static_cast<uint32_t>(request.arguments.size()));
        for (const auto &argument : request.arguments)
            w.String(argument);
        return {MessageType::CALL, w.Take()};
    }

    // Each result: u8 success, string message, u32 column count, the column
    // names, u32 row count, each row as a string of Tuple bytes
    Frame Encode(const ResultsResponse &response)
    {
        Writer w;
        w.U8(response.in_transaction ? 1 : 0);
        w.U32(static_cast<uint32_t>(response.results.size()));
        for (const auto &result : response.results)
        {
            w.U8(result.success ? 1 : 0);
            w.String(result.message);
            w.U32(static_cast<uint32_t>(result.column_names.size()));
            for (const auto &name : result.column_names)
                w.String(name);
            w.U32(static_cast<uint32_t>(result.tuples.size()));
            for (const auto &tuple : result.tuples)
            {
                std::string row;
                tuple.SerializeTo(&row);
                w.String(row);
            }
        }
        return {MessageType::RESULTS, w.Take()};
    }

    Frame Encode(const TextResponse &response)
    {
        Writer w;
        w.U8(response.in_transaction ? 1 : 0);
        w.String(response.text);
        return {MessageType::TEXT, w.Take()};
    }

    Frame EncodeError(const std::string &message)
    {
        Writer w;
        w.String(message);
        return {MessageType::ERROR, w.Take()};
    }

    std::optional<ExecuteRequest> DecodeExecute(const Frame &frame)
    {
        if (frame.type != MessageType::EXECUTE)
            return std::nullopt;
        Reader r(frame.payload);
        ExecuteRequest request;
        uint8_t mode = 0;
        if (!r.U8(&mode) || mode > static_cast<uint8_t>(ExecuteMode::SCRIPT_STOP_ON_ERROR) ||
            !r.String(&request.sql) || !r.AtEnd())
            return std::nullopt;
        request.mode = static_cast<ExecuteMode>(mode);
        return request;
    }

    std::optional<CommandRequest> DecodeCommand(const Frame &frame)
    {
        if (frame.type != MessageType::COMMAND)
            return std::nullopt;
        Reader r(frame.payload);
        CommandRequest request;
        if (!r.String(&request.name) || !r.String(&request.argument) || !r.AtEnd())
            return std::nullopt;
        return request;
    }

    std::optional<CallRequest> DecodeCall(const Frame &frame)
    {
        if (frame.type != MessageType::CALL)
            return std::nullopt;
        Reader r(frame.payload);
        CallRequest request;
        uint32_t count = 0;
        if (!r.String(&request.method) || !r.U32(&count))
            return std::nullopt;
        for (uint32_t i = 0; i < count; ++i)
        {
            std::string argument;
            if (!r.String(&argument))
                return std::nullopt;
            request.arguments.push_back(std::move(argument));
        }
        if (!r.AtEnd())
            return std::nullopt;
        return request;
    }

    std::optional<ResultsResponse> DecodeResults(const Frame &frame)
    {
        if (frame.type != MessageType::RESULTS)
            return std::nullopt;
        Reader r(frame.payload);
        ResultsResponse response;
        uint8_t in_transaction = 0;
        uint32_t count = 0;
        if (!r.U8(&in_transaction) || !r.U32(&count))
            return std::nullopt;
        response.in_transaction = in_transaction != 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            ExecutionResult result;
            uint8_t success = 0;
            uint32_t columns = 0, rows = 0;
            if (!r.U8(&success) || !r.String(&result.message) || !r.U32(&columns))
                return std::nullopt;
            result.success = success != 0;
            for (uint32_t c = 0; c < columns; ++c)
            {
                std::string name;
                if (!r.String(&name))
                    return std::nullopt;
                result.column_names.push_back(std::move(name));
            }
            if (!r.U32(&rows))
                return std::nullopt;
            for (uint32_t row = 0; row < rows; ++row)
            {
                std::string bytes;
                Tuple tuple;
                if (!r.String(&bytes) || !Tuple::DeserializeFrom(bytes.data(), bytes.size(), &tuple))
                    return std::nullopt;
                result.tuples.push_back(std::move(tuple));
            }
            response.results.push_back(std::move(result));
        }
        if (!r.AtEnd())
            return std::nullopt;
        return response;
    }

    std::optional<TextResponse> DecodeText(const Frame &frame)
    {
        if (frame.type != MessageType::TEXT)
            return std::nullopt;
        Reader r(frame.payload);
        TextResponse response;
        uint8_t in_transaction = 0;
        if (!r.U8(&in_transaction) || !r.String(&response.text) || !r.AtEnd())
            return std::nullopt;
        response.in_transaction = in_transaction != 0;
        return response;
    }

    std::optional<std::string> DecodeError(const Frame &frame)
    {
        if (frame.type != MessageType::ERROR)
            return std::nullopt;
        Reader r(frame.payload);
        std::string message;
        if (!r.String(&message) || !r.AtEnd())
            return std::nullopt;
        return message;
    }

} // namespace sql::net
