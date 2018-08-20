/* Some modifications Copyright (c) 2018 BlackBerry Limited

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#pragma once

#include <DataStreams/copyData.h>
#include <DataStreams/IBlockOutputStream.h>
#include <DataStreams/OneBlockInputStream.h>
#include <DataStreams/MaterializingBlockInputStream.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Storages/StorageMaterializedView.h>
#include <Storages/StorageLiveView.h>
#include <Storages/StorageLiveChannel.h>


namespace DB
{

class ReplicatedMergeTreeBlockOutputStream;


/** Writes data to the specified table and to all dependent materialized views.
  */
class PushingToViewsBlockOutputStream : public IBlockOutputStream
{
public:
    PushingToViewsBlockOutputStream(String database, String table, const Context & context_, const ASTPtr & query_ptr_, bool no_destination = false);

    void write(const Block & block) override;

    void flush() override
    {
        if (output)
            output->flush();
    }

    void writePrefix() override
    {
        if (output)
            output->writePrefix();
    }

    void writeSuffix() override
    {
        if (output)
            output->writeSuffix();
    }

private:

    StoragePtr storage;
    BlockOutputStreamPtr output;
    ReplicatedMergeTreeBlockOutputStream * replicated_output = nullptr;

    const Context & context;
    ASTPtr query_ptr;

    std::vector<std::pair<StoragePtr, BlockOutputStreamPtr>> views;
    std::unique_ptr<Context> views_context;
};


}
