#include <cassert>
#include <Common/Exception.h>

#include <DataStreams/ConvertingBlockInputStream.h>
#include <DataStreams/IBlockInputStream.h>
#include <DataStreams/MaterializingBlockInputStream.h>
#include <DataStreams/OneBlockInputStream.h>
#include <DataStreams/PushingToViewsBlockOutputStream.h>
#include <DataStreams/SquashingBlockInputStream.h>

#include <DataTypes/NestedUtils.h>
#include <Interpreters/ExpressionAnalyzer.h>
#include <Interpreters/MutationsInterpreter.h>
#include <Interpreters/TreeRewriter.h>
#include <Interpreters/JoinedTables.h>
#include <Storages/StorageAggregatingMemory.h>
#include <Storages/StorageFactory.h>
#include <Storages/StorageValues.h>

#include <IO/WriteHelpers.h>
#include <Processors/Pipe.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/Sources/SourceWithProgress.h>
#include <Processors/Transforms/AggregatingTransform.cpp>
#include <Processors/Transforms/AggregatingTransform.h>
#include <Processors/Transforms/ExpressionTransform.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int INCORRECT_QUERY;
}

void executeExpression(Pipe & pipe, const ActionsDAGPtr & expression)
{
    if (!expression)
    {
        return;
    }

    auto expression_actions = std::make_shared<ExpressionActions>(expression);

    pipe.addSimpleTransform([expression_actions](const Block & header)
    {
        return std::make_shared<ExpressionTransform>(header, expression_actions);
    });
}

/// AggregatingOutputStream is used to feed data into Aggregator.
class AggregatingOutputStream : public IBlockOutputStream
{
public:
    AggregatingOutputStream(StorageAggregatingMemory & storage_, const StorageMetadataPtr & metadata_snapshot_, ContextPtr context_)
        : storage(storage_),
          metadata_snapshot(metadata_snapshot_),
          context(context_),
          variants(*(storage.many_data)->variants[0]),
          key_columns(storage.aggregator_transform->params.keys_size),
          aggregate_columns(storage.aggregator_transform->params.aggregates_size)
    {
        expression_actions = std::make_shared<ExpressionActions>(storage.analysis_result.before_aggregation);
    }

    // OutputStream structure is same as source (before aggregation).
    Block getHeader() const override { return storage.src_block_header; }

    void write(const Block & block) override
    {
        storage.src_metadata_snapshot->check(block, true);

        Block block_for_aggregation(block);
        expression_actions->execute(block_for_aggregation);

        bool no_more_keys = false;
        storage.aggregator_transform->aggregator.executeOnBlock(
            block_for_aggregation, variants, key_columns, aggregate_columns, no_more_keys);
    }

    // Used to run aggregation the usual way (via InterpreterSelectQuery),
    // and only purpose is to aid development.
    // TODO remove this.
    void writeForDebug(const Block & block)
    {
        BlockInputStreamPtr in;

        std::optional<InterpreterSelectQuery> select;

        if (metadata_snapshot->hasSelectQuery())
        {
            auto query = metadata_snapshot->getSelectQuery();

            auto block_storage
                = StorageValues::create(storage.getStorageID(), metadata_snapshot->getColumns(), block, storage.getVirtuals());

            ContextPtr local_context = context;
            local_context->addViewSource(block_storage);

            auto select_query = query.inner_query->as<ASTSelectQuery &>();

            LOG_DEBUG(&Poco::Logger::get("Arthur"), "executing debug select query");
            select.emplace(query.inner_query, local_context, SelectQueryOptions());
            auto select_result = select->execute();

            in = std::make_shared<MaterializingBlockInputStream>(select_result.getInputStream());

            in = std::make_shared<SquashingBlockInputStream>(
                in, context->getSettingsRef().min_insert_block_size_rows, context->getSettingsRef().min_insert_block_size_bytes);
            in = std::make_shared<ConvertingBlockInputStream>(
                in, metadata_snapshot->getSampleBlock(), ConvertingBlockInputStream::MatchColumnsMode::Name);
        }
        else
            in = std::make_shared<OneBlockInputStream>(block);

        in->readPrefix();

        while (Block result_block = in->read())
        {
            Nested::validateArraySizes(result_block);
        }

        in->readSuffix();
    }

private:
    StorageAggregatingMemory & storage;
    StorageMetadataPtr metadata_snapshot;
    ContextPtr context;

    AggregatedDataVariants & variants;
    ColumnRawPtrs key_columns;
    Aggregator::AggregateColumns aggregate_columns;

    ExpressionActionsPtr expression_actions;
};


StorageAggregatingMemory::StorageAggregatingMemory(
    const StorageID & table_id_,
    ConstraintsDescription constraints_,
    const ASTCreateQuery & query,
    ContextPtr context_)
    : IStorage(table_id_)
{
    if (!query.select)
        throw Exception("SELECT query is not specified for " + getName(), ErrorCodes::INCORRECT_QUERY);

    if (query.select->list_of_selects->children.size() != 1)
        throw Exception("UNION is not supported for AggregatingMemory", ErrorCodes::INCORRECT_QUERY);

    // TODO check validity of aggregation query inside this func
    auto select = SelectQueryDescription::getSelectQueryFromASTForAggr(query.select->clone());
    ASTPtr select_ptr = select.inner_query;

    auto select_context = std::make_unique<ContextPtr>(context_);

    /// Get info about source table.
    JoinedTables joined_tables(context_, select_ptr->as<ASTSelectQuery &>());
    StoragePtr source_storage = joined_tables.getLeftTableStorage();
    NamesAndTypesList source_columns = source_storage->getInMemoryMetadata().getColumns().getAll();

    ColumnsDescription columns_before_aggr;
    for (const auto & column : source_columns)
    {
        ColumnDescription column_description(column.name, column.type);
        columns_before_aggr.add(column_description);
    }

    /// Get list of columns we get from select query.
    Block header = InterpreterSelectQuery(select_ptr, *select_context, SelectQueryOptions().analyze()).getSampleBlock();

    ColumnsDescription columns_after_aggr;

    /// Insert only columns returned by select.
    for (const auto & column : header)
    {
        ColumnDescription column_description(column.name, column.type);
        columns_after_aggr.add(column_description);
    }

    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(std::move(columns_after_aggr));
    storage_metadata.setConstraints(std::move(constraints_));
    storage_metadata.setSelectQuery(std::move(select));
    setInMemoryMetadata(storage_metadata);

    StorageInMemoryMetadata src_metadata;
    src_metadata.setColumns(std::move(columns_before_aggr));
    src_block_header = src_metadata.getSampleBlock();

    src_metadata_snapshot = std::make_shared<StorageInMemoryMetadata>(src_metadata);

    Names required_result_column_names;

    auto syntax_analyzer_result
        = TreeRewriter(*select_context)
              .analyzeSelect(
                  select_ptr, TreeRewriterResult(src_block_header.getNamesAndTypesList()), {}, {}, required_result_column_names, {});

    auto query_analyzer = std::make_unique<SelectQueryExpressionAnalyzer>(
        select_ptr,
        syntax_analyzer_result,
        *select_context,
        src_metadata_snapshot,
        NameSet(required_result_column_names.begin(), required_result_column_names.end()));

    const Settings & settings = (*select_context)->getSettingsRef();

    analysis_result = ExpressionAnalysisResult(*query_analyzer, src_metadata_snapshot, false, false, false, nullptr, src_block_header);

    Block header_before_aggregation = src_block_header;
    auto expression = analysis_result.before_aggregation;
    auto expression_actions = std::make_shared<ExpressionActions>(expression);
    expression_actions->execute(header_before_aggregation);

    ColumnNumbers keys;
    for (const auto & key : query_analyzer->aggregationKeys())
        keys.push_back(header_before_aggregation.getPositionByName(key.name));

    AggregateDescriptions aggregates = query_analyzer->aggregates();
    for (auto & descr : aggregates)
        if (descr.arguments.empty())
            for (const auto & name : descr.argument_names)
                descr.arguments.push_back(header_before_aggregation.getPositionByName(name));

    Aggregator::Params params(header_before_aggregation, keys, aggregates,
                              false, settings.max_rows_to_group_by, settings.group_by_overflow_mode,
                              settings.group_by_two_level_threshold,
                              settings.group_by_two_level_threshold_bytes,
                              settings.max_bytes_before_external_group_by,
                              settings.empty_result_for_aggregation_by_empty_set,
                              (*select_context)->getTemporaryVolume(),
                              settings.max_threads,
                              settings.min_free_disk_space_for_temporary_data,
                              true);

    aggregator_transform = std::make_shared<AggregatingTransformParams>(params, true);
    many_data = std::make_shared<ManyAggregatedData>(1);

    /// If there was no data, and we aggregate without keys, and we must return single row with the result of empty aggregation.
    /// To do this, we pass a block with zero rows to aggregate.
    if (params.keys_size == 0 && !params.empty_result_for_aggregation_by_empty_set)
    {
        AggregatingOutputStream os(*this, getInMemoryMetadataPtr(), context_);
        os.write(src_block_header);
    }
}


Pipe StorageAggregatingMemory::read(
    const Names & column_names,
    const StorageMetadataPtr & metadata_snapshot,
    SelectQueryInfo & /*query_info*/,
    ContextPtr /*context*/,
    QueryProcessingStage::Enum /*processed_stage*/,
    size_t /*max_block_size*/,
    unsigned num_streams)
{
    metadata_snapshot->check(column_names, getVirtuals(), getStorageID());

    // TODO allow parallel read (num_streams)
    // TODO implement O(1) read by aggregation key

    auto prepared_data = aggregator_transform->aggregator.prepareVariantsToMerge(many_data->variants);
    auto prepared_data_ptr = std::make_shared<ManyAggregatedDataVariants>(std::move(prepared_data));

    auto processor
        = std::make_shared<ConvertingAggregatedToChunksTransform>(aggregator_transform, std::move(prepared_data_ptr), num_streams);

    Pipe pipe(std::move(processor));
    executeExpression(pipe, analysis_result.before_window);
    // TODO add support for window expressions
    executeExpression(pipe, analysis_result.before_order_by);
    executeExpression(pipe, analysis_result.final_projection);
    // TODO implement ORDER BY? (quite hard)

    return pipe;
}

BlockOutputStreamPtr StorageAggregatingMemory::write(const ASTPtr & /*query*/, const StorageMetadataPtr & metadata_snapshot, ContextPtr context)
{
    auto out = std::make_shared<AggregatingOutputStream>(*this, metadata_snapshot, context);
    return out;
}

void StorageAggregatingMemory::drop()
{
    // TODO drop aggregator state
}

void StorageAggregatingMemory::truncate(const ASTPtr &, const StorageMetadataPtr &, ContextPtr, TableExclusiveLockHolder &)
{
    // TODO clear aggregator state
}

void registerStorageAggregatingMemory(StorageFactory & factory)
{
    factory.registerStorage("AggregatingMemory", [](const StorageFactory::Arguments & args)
    {
        if (!args.engine_args.empty())
            throw Exception(
                "Engine " + args.engine_name + " doesn't support any arguments (" + toString(args.engine_args.size()) + " given)",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        return StorageAggregatingMemory::create(args.table_id, args.constraints, args.query, args.getLocalContext());
    },
    {
        .supports_parallel_insert = true, // TODO not sure
    });
}

}
