#include <Storages/RabbitMQ/ReadBufferFromRabbitMQConsumer.h>
#include <common/logger_useful.h>
#include <amqpcpp.h>

namespace DB
{

ReadBufferFromRabbitMQConsumer::ReadBufferFromRabbitMQConsumer(
        ConsumerPtr consumer_,
        RabbitMQHandler * handler_)
        : ReadBuffer(nullptr, 0)
        , consumer(consumer_)
        , handler(handler_)
        , current(messages.begin())
{
}

/// this is to be modified

ReadBufferFromRabbitMQConsumer::~ReadBufferFromRabbitMQConsumer() = default;

void ReadBufferFromRabbitMQConsumer::commit()
{
    handler->loop();
}

void ReadBufferFromRabbitMQConsumer::subscribe(const Names & routing_keys)
{
    for (auto key : routing_keys)
    {
        consumer->declareQueue(key);
        consumer->consume(key, AMQP::noack).onReceived(
                [](const AMQP::Message & /* message */,
                   uint64_t /* deliveryTag */,
                   bool /* redelivered */)
                {
                    /** std::string  message =
                            "Received message with deliveryTag" + std::to_string(deliveryTag);
                    LOG_TRACE(log, message); */
                });
    }
}

void ReadBufferFromRabbitMQConsumer::unsubscribe()
{
    messages.clear();
    current = messages.begin();
    BufferBase::set(nullptr, 0, 0);

    handler->quit();
}
}
