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
#include <DataStreams/IProfilingBlockInputStream.h>
#include <DataStreams/IBlockOutputStream.h>
#include <DataStreams/copyData.h>


namespace DB
{

namespace
{

bool isAtomicSet(std::atomic<bool> * val)
{
    return ((val != nullptr) && val->load(std::memory_order_seq_cst));
}

}

void copyData(IBlockInputStream & from, IBlockOutputStream & to, std::atomic<bool> * is_cancelled)
{
    from.readPrefix();
    /// Set to stream frame as opened
    bool open_frame = false;
    bool no_data = true;

    while (Block block = from.read())
    {
        no_data = false;

        if (isAtomicSet(is_cancelled))
            break;

        if (!open_frame || block.info.is_start_frame)
        {
            to.setSampleBlock(block);
            to.writePrefix();
            open_frame = true;
        }

        to.write(block);

        /// If this block is end of frame then
        /// then close current frame
        if (block.info.is_end_frame)
        {
            to.writeSuffix();
            open_frame = false;
        }
    }

    if ( no_data )
    {
        to.writePrefix();
        open_frame = true;
    }

    if (isAtomicSet(is_cancelled))
        return;

    /// For outputting additional information in some formats.
    if (IProfilingBlockInputStream * input = dynamic_cast<IProfilingBlockInputStream *>(&from))
    {
        if (input->getProfileInfo().hasAppliedLimit())
            to.setRowsBeforeLimit(input->getProfileInfo().getRowsBeforeLimit());

        to.setTotals(input->getTotals());
        to.setExtremes(input->getExtremes());
    }

    if (isAtomicSet(is_cancelled))
        return;

    from.readSuffix();

    /// Close frame if it is opened
    if (open_frame)
        to.writeSuffix();
}

}
