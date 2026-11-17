#include <gtest/gtest.h>
#include "net/protocol.h"
#include "net/socket.h"
#include <cmath>
#include <sys/socket.h>
#include <unistd.h>

namespace sql::net
{

    namespace
    {
        // A connected pair of sockets in this process
        std::pair<Socket, Socket> Pair()
        {
            int fds[2];
            EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
            return {Socket(fds[0]), Socket(fds[1])};
        }

        void SendRaw(const Socket &socket, const std::string &bytes)
        {
            ASSERT_EQ(::send(socket.Fd(), bytes.data(), bytes.size(), 0), static_cast<ssize_t>(bytes.size()));
        }
    } // namespace

    TEST(ProtocolTest, FramesAreLengthPrefixedBigEndian)
    {
        const std::string bytes = EncodeFrame({MessageType::TEXT, "abc"});
        EXPECT_EQ(bytes, std::string("\x00\x00\x00\x04\x04"
                                     "abc",
                                     8));
    }

    TEST(ProtocolTest, RequestsRoundTrip)
    {
        for (auto mode : {ExecuteMode::STATEMENT, ExecuteMode::SCRIPT, ExecuteMode::SCRIPT_STOP_ON_ERROR})
        {
            auto decoded = DecodeExecute(Encode(ExecuteRequest{mode, "SELECT 'it''s';"}));
            ASSERT_TRUE(decoded);
            EXPECT_EQ(decoded->mode, mode);
            EXPECT_EQ(decoded->sql, "SELECT 'it''s';");
        }
        auto command = DecodeCommand(Encode(CommandRequest{"schema", "t"}));
        ASSERT_TRUE(command);
        EXPECT_EQ(command->name, "schema");
        EXPECT_EQ(command->argument, "t");
        // A frame decodes only as its own type
        EXPECT_FALSE(DecodeCommand(Encode(ExecuteRequest{ExecuteMode::STATEMENT, "x"})));
        EXPECT_FALSE(DecodeResults(Encode(TextResponse{false, "x"})));
    }

    TEST(ProtocolTest, ResultsRoundTripEveryValueType)
    {
        ResultsResponse response;
        response.in_transaction = true;
        ExecutionResult rows;
        rows.success = true;
        rows.message = "3 row(s)";
        rows.column_names = {"i", "f", "s", "b", "n"};
        rows.tuples.push_back(Tuple({Value(-7), Value(2.5), Value(std::string("a\0b", 3)), Value(true),
                                     Value(DataType::INTEGER)}));
        rows.tuples.push_back(Tuple({Value(INT32_MAX), Value(-0.0), Value(""), Value(false), Value(DataType::VARCHAR)}));
        ExecutionResult failure;
        failure.success = false;
        failure.message = "Unknown column: x";
        response.results = {rows, failure, ExecutionResult{true, "", {}, {}}};

        auto decoded = DecodeResults(Encode(response));
        ASSERT_TRUE(decoded);
        EXPECT_TRUE(decoded->in_transaction);
        ASSERT_EQ(decoded->results.size(), 3u);
        const ExecutionResult &got = decoded->results[0];
        EXPECT_TRUE(got.success);
        EXPECT_EQ(got.message, "3 row(s)");
        EXPECT_EQ(got.column_names, rows.column_names);
        ASSERT_EQ(got.tuples.size(), 2u);
        for (size_t r = 0; r < 2; ++r)
        {
            ASSERT_EQ(got.tuples[r].GetValueCount(), 5u);
            for (size_t c = 0; c < 5; ++c)
            {
                const Value &a = got.tuples[r].GetValue(c), &b = rows.tuples[r].GetValue(c);
                EXPECT_EQ(a.IsNull(), b.IsNull()) << r << "," << c;
                EXPECT_EQ(a.GetType(), b.GetType()) << r << "," << c;
                if (!a.IsNull())
                {
                    EXPECT_EQ(a.ToString(), b.ToString()) << r << "," << c;
                }
            }
        }
        EXPECT_TRUE(std::signbit(got.tuples[1].GetValue(1).GetAsFloat())); // -0.0 kept
        EXPECT_FALSE(decoded->results[1].success);
        EXPECT_EQ(decoded->results[1].message, "Unknown column: x");
    }

    TEST(ProtocolTest, MalformedPayloadsAreRejected)
    {
        ResultsResponse response;
        ExecutionResult result;
        result.success = true;
        result.column_names = {"a"};
        result.tuples.push_back(Tuple({Value(1)}));
        response.results = {result};
        const Frame good = Encode(response);
        // Every truncation fails, and so do trailing bytes
        for (size_t size = 0; size < good.payload.size(); ++size)
            EXPECT_FALSE(DecodeResults({MessageType::RESULTS, good.payload.substr(0, size)})) << size;
        EXPECT_FALSE(DecodeResults({MessageType::RESULTS, good.payload + "x"}));
        // An unknown execute mode
        EXPECT_FALSE(DecodeExecute({MessageType::EXECUTE, std::string("\x09\x00\x00\x00\x00", 5)}));
        // A string claiming more bytes than there are
        EXPECT_FALSE(DecodeError({MessageType::ERROR, std::string("\xff\xff\xff\xff", 4)}));
    }

    TEST(ProtocolTest, FramesCrossASocket)
    {
        auto [a, b] = Pair();
        WriteFrame(a, Encode(CommandRequest{"tables", ""}));
        WriteFrame(a, EncodeError("boom"));
        Frame frame;
        ASSERT_TRUE(ReadFrame(b, &frame));
        EXPECT_TRUE(DecodeCommand(frame));
        ASSERT_TRUE(ReadFrame(b, &frame));
        EXPECT_EQ(DecodeError(frame), std::optional<std::string>("boom"));
        // A clean close between frames is the end of the stream
        a.Close();
        EXPECT_FALSE(ReadFrame(b, &frame));
    }

    TEST(ProtocolTest, BrokenStreamsThrow)
    {
        Frame frame;
        {
            auto [a, b] = Pair();
            SendRaw(a, std::string("\x00\x00", 2)); // half a header
            a.Close();
            EXPECT_THROW(ReadFrame(b, &frame), std::runtime_error);
        }
        {
            auto [a, b] = Pair();
            SendRaw(a, std::string("\x00\x00\x00\x09\x04"
                                   "abc",
                                   8)); // promises 8 payload bytes, sends 3
            a.Close();
            EXPECT_THROW(ReadFrame(b, &frame), std::runtime_error);
        }
        {
            auto [a, b] = Pair();
            SendRaw(a, std::string("\x7f\xff\xff\xff\x04", 5)); // over MAX_FRAME_SIZE
            EXPECT_THROW(ReadFrame(b, &frame), std::runtime_error);
        }
        {
            auto [a, b] = Pair();
            SendRaw(a, std::string("\x00\x00\x00\x00\x04", 5)); // length 0: no room for the type
            EXPECT_THROW(ReadFrame(b, &frame), std::runtime_error);
        }
        {
            auto [a, b] = Pair();
            b.Close();
            EXPECT_THROW(WriteFrame(a, EncodeError("nobody listening")), std::runtime_error);
        }
    }

} // namespace sql::net
