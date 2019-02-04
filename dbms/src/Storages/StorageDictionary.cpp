#include <sstream>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeDate.h>
#include <Dictionaries/IDictionarySource.h>
#include <Dictionaries/DictionaryStructure.h>
#include <Storages/StorageDictionary.h>
#include <Storages/StorageFactory.h>
#include <Interpreters/Context.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Interpreters/ExternalDictionaries.h>
#include <Parsers/ASTLiteral.h>
#include <common/logger_useful.h>
#include <Common/typeid_cast.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int THERE_IS_NO_COLUMN;
}


StorageDictionary::StorageDictionary(
    const String & table_name_,
    const String & database_name_,
    const ColumnsDescription & columns_,
    std::optional<std::reference_wrapper<const DictionaryStructure>> dictionary_structure_,
    const String & dictionary_name_,
    bool attach_)
    : IStorage{columns_}
    , table_name(table_name_)
    , database_name(database_name_)
    , dictionary_name(dictionary_name_)
    , logger(&Poco::Logger::get("StorageDictionary"))
{
    if (!attach_)
    {
        checkNamesAndTypesCompatibleWithDictionary(*dictionary_structure_);
    }
}

BlockInputStreams StorageDictionary::read(
    const Names & column_names,
    const SelectQueryInfo & /*query_info*/,
    const Context & context,
    QueryProcessingStage::Enum /*processed_stage*/,
    const size_t max_block_size,
    const unsigned /*threads*/)
{
    DictionaryPtr dictionary;
    if (database_name.empty())
    {
        dictionary = context.getExternalDictionaries().getDictionary(dictionary_name);
    }
    else
    {
        auto db = context.getDatabase(database_name);
        dictionary = db->getDictionary(context, dictionary_name);
    }

    return BlockInputStreams{dictionary->getBlockInputStream(column_names, max_block_size)};
}

NamesAndTypesList StorageDictionary::getNamesAndTypes(const DictionaryStructure & dictionary_structure)
{
    NamesAndTypesList dictionary_names_and_types;

    if (dictionary_structure.id)
        dictionary_names_and_types.emplace_back(dictionary_structure.id->name, std::make_shared<DataTypeUInt64>());
    if (dictionary_structure.range_min)
        dictionary_names_and_types.emplace_back(dictionary_structure.range_min->name, dictionary_structure.range_min->type);
    if (dictionary_structure.range_max)
        dictionary_names_and_types.emplace_back(dictionary_structure.range_max->name, dictionary_structure.range_max->type);
    if (dictionary_structure.key)
        for (const auto & attribute : *dictionary_structure.key)
            dictionary_names_and_types.emplace_back(attribute.name, attribute.type);

    for (const auto & attribute : dictionary_structure.attributes)
        dictionary_names_and_types.emplace_back(attribute.name, attribute.type);

    return dictionary_names_and_types;
}

void StorageDictionary::checkNamesAndTypesCompatibleWithDictionary(const DictionaryStructure & dictionary_structure) const
{
    auto dictionary_names_and_types = getNamesAndTypes(dictionary_structure);
    std::set<NameAndTypePair> namesAndTypesSet(dictionary_names_and_types.begin(), dictionary_names_and_types.end());

    for (auto & column : getColumns().ordinary)
    {
        if (namesAndTypesSet.find(column) == namesAndTypesSet.end())
        {
            std::string message = "Not found column ";
            message += column.name + " " + column.type->getName();
            message += " in dictionary " + dictionary_name + ". ";
            message += "There are only columns ";
            message += generateNamesAndTypesDescription(dictionary_names_and_types.begin(), dictionary_names_and_types.end());
            throw Exception(message, ErrorCodes::THERE_IS_NO_COLUMN);
        }
    }
}

void registerStorageDictionary(StorageFactory & factory)
{
    factory.registerStorage("Dictionary", [](const StorageFactory::Arguments & args)
    {
        if (args.engine_args.size() != 1)
            throw Exception("Storage Dictionary requires single parameter: name of dictionary",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        args.engine_args[0] = evaluateConstantExpressionOrIdentifierAsLiteral(args.engine_args[0], args.local_context);
        String database_name;
        String dictionary_name = typeid_cast<const ASTLiteral &>(*args.engine_args[0]).value.safeGet<String>();
        if (auto pos = dictionary_name.find('.'); pos != String::npos)
        {
            database_name = dictionary_name.substr(0, pos);
            dictionary_name = dictionary_name.substr(pos + 1);
        }

        if (args.attach)
        {
            DictionaryStructure empty_structure;
            return StorageDictionary::create(
                    args.table_name, database_name, args.columns, std::cref(empty_structure), dictionary_name, true);
        }

        DictionaryPtr dictionary;
        if (database_name.empty())
        {
            dictionary = args.context.getExternalDictionaries().getDictionary(dictionary_name);
        }
        else
        {
            auto db = args.context.getDatabase(database_name);
            dictionary = db->getDictionary(args.context, dictionary_name);
        }

        const DictionaryStructure & dictionary_structure = dictionary->getStructure();
        return StorageDictionary::create(
            args.table_name, database_name, args.columns, std::cref(dictionary_structure), dictionary_name, false);
    });
}

}
