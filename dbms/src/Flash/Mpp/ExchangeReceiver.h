#pragma once

#include <DataStreams/IProfilingBlockInputStream.h>
#include <Flash/Coprocessor/ArrowChunkCodec.h>
#include <Flash/Coprocessor/CHBlockChunkCodec.h>
#include <Flash/Coprocessor/DefaultChunkCodec.h>
#include <Interpreters/Context.h>
#include <Storages/Transaction/TMTContext.h>
#include <common/logger_useful.h>

#include <chrono>
#include <mutex>
#include <thread>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <kvproto/mpp.pb.h>
#include <pingcap/kv/Rpc.h>
#include <tipb/executor.pb.h>
#include <tipb/select.pb.h>

#pragma GCC diagnostic pop


namespace DB
{

class ExchangeReceiver
{
    TMTContext & context;
    std::chrono::seconds timeout;

    tipb::ExchangeReceiver pb_exchange_receiver;
    ::mpp::TaskMeta task_meta;
    std::vector<std::thread> workers;
    // async grpc
    grpc::CompletionQueue grpc_com_queue;
    DAGSchema schema;

    // TODO: should be a concurrency bounded queue.
    std::mutex rw_mu;
    std::condition_variable cv;
    std::queue<Block> block_buffer;
    std::atomic_int live_connections;
    bool inited;
    bool meet_error;
    Exception err;
    Logger * log;
    class ExchangeCall;

    // All calls should live until the receiver shuts down
    std::vector<std::shared_ptr<ExchangeCall>> exchangeCalls;
    void sendAsyncReq();

    void proceedAsyncReq();

    void decodePacket(const mpp::MPPDataPacket & p)
    {
        tipb::SelectResponse resp;
        resp.ParseFromString(p.data());
        int chunks_size = resp.chunks_size();
        LOG_DEBUG(log, "get chunk size " + std::to_string(chunks_size));
        if (chunks_size == 0)
            return;
        for (int i = 0; i < chunks_size; i++)
        {
            Block block;
            const tipb::Chunk & chunk = resp.chunks(i);
            switch (resp.encode_type())
            {
                case tipb::EncodeType::TypeCHBlock:
                    block = CHBlockChunkCodec().decode(chunk, schema);
                    break;
                case tipb::EncodeType::TypeChunk:
                    block = ArrowChunkCodec().decode(chunk, schema);
                    break;
                case tipb::EncodeType::TypeDefault:
                    block = DefaultChunkCodec().decode(chunk, schema);
                    break;
                default:
                    throw Exception("Unsupported encode type", ErrorCodes::LOGICAL_ERROR);
            }
            std::lock_guard<std::mutex> lk(rw_mu);
            block_buffer.push(std::move(block));
            cv.notify_one();
        }
    }

public:
    ExchangeReceiver(Context & context_, const ::tipb::ExchangeReceiver & exc, const ::mpp::TaskMeta & meta)
        : context(context_.getTMTContext()),
          timeout(context_.getSettings().mpp_task_timeout),
          pb_exchange_receiver(exc),
          task_meta(meta),
          live_connections(0),
          inited(false),
          meet_error(false),
          log(&Logger::get("exchange_receiver"))
    {
        for (int i = 0; i < exc.field_types_size(); i++)
        {
            String name = "exchange_receiver_" + std::to_string(i);
            ColumnInfo info = fieldTypeToColumnInfo(exc.field_types(i));
            schema.push_back(std::make_pair(name, info));
        }
    }

    ~ExchangeReceiver()
    {
        for (auto & worker : workers)
        {
            worker.join();
        }
        exchangeCalls.clear();
        grpc_com_queue.Shutdown();
    }

    const DAGSchema & getOutputSchema() const { return schema; }

    void init()
    {
        std::lock_guard<std::mutex> lk(rw_mu);
        if (!inited)
        {
            sendAsyncReq();
            workers.emplace_back(std::thread(&ExchangeReceiver::proceedAsyncReq, this));
            inited = true;
        }
    }

    Block nextBlock()
    {
        if (!inited)
            init();
        std::unique_lock<std::mutex> lk(rw_mu);
        cv.wait(lk, [&] { return block_buffer.size() > 0 || live_connections == 0 || meet_error; });
        if (meet_error)
        {
            throw err;
        }
        if (block_buffer.empty())
        {
            return {};
        }
        Block blk = block_buffer.front();
        block_buffer.pop();
        return blk;
    }
};
} // namespace DB