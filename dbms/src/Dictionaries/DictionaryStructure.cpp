#include "DictionaryStructure.h"
#include <Formats/FormatSettings.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeNullable.h>
#include <Columns/IColumn.h>
#include <Common/typeid_cast.h>
#include <Common/StringUtils/StringUtils.h>
#include <IO/WriteHelpers.h>
#include <Parsers/ASTColumnDeclaration.h>
#include <Parsers/queryToString.h>

#include <Poco/DOM/Document.h>
#include <Poco/DOM/Element.h>
#include <Poco/DOM/Text.h>
#include <Poco/DOM/AutoPtr.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Poco/Util/XMLConfiguration.h>

#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <ext/range.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int UNKNOWN_TYPE;
    extern const int ARGUMENT_OUT_OF_BOUND;
    extern const int TYPE_MISMATCH;
    extern const int BAD_ARGUMENTS;
    extern const int CANNOT_CONSTRUCT_CONFIGURATION_FROM_AST;
}

namespace
{
    DictionaryTypedSpecialAttribute makeDictionaryTypedSpecialAttribute(
        const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix, const std::string & default_type)
    {
        const auto name = config.getString(config_prefix + ".name", "");
        const auto expression = config.getString(config_prefix + ".expression", "");

        if (name.empty() && !expression.empty())
            throw Exception{"Element " + config_prefix + ".name is empty", ErrorCodes::BAD_ARGUMENTS};

        const auto type_name = config.getString(config_prefix + ".type", default_type);
        return DictionaryTypedSpecialAttribute{std::move(name), std::move(expression), DataTypeFactory::instance().get(type_name)};
    }

} // namespace


bool isAttributeTypeConvertibleTo(AttributeUnderlyingType from, AttributeUnderlyingType to)
{
    if (from == to)
        return true;

    /** This enum can be somewhat incomplete and the meaning may not coincide with NumberTraits.h.
      * (for example, because integers can not be converted to floats)
      * This is normal for a limited usage scope.
      */
    if ((from == AttributeUnderlyingType::UInt8 && to == AttributeUnderlyingType::UInt16)
        || (from == AttributeUnderlyingType::UInt8 && to == AttributeUnderlyingType::UInt32)
        || (from == AttributeUnderlyingType::UInt8 && to == AttributeUnderlyingType::UInt64)
        || (from == AttributeUnderlyingType::UInt16 && to == AttributeUnderlyingType::UInt32)
        || (from == AttributeUnderlyingType::UInt16 && to == AttributeUnderlyingType::UInt64)
        || (from == AttributeUnderlyingType::UInt32 && to == AttributeUnderlyingType::UInt64)
        || (from == AttributeUnderlyingType::UInt8 && to == AttributeUnderlyingType::Int16)
        || (from == AttributeUnderlyingType::UInt8 && to == AttributeUnderlyingType::Int32)
        || (from == AttributeUnderlyingType::UInt8 && to == AttributeUnderlyingType::Int64)
        || (from == AttributeUnderlyingType::UInt16 && to == AttributeUnderlyingType::Int32)
        || (from == AttributeUnderlyingType::UInt16 && to == AttributeUnderlyingType::Int64)
        || (from == AttributeUnderlyingType::UInt32 && to == AttributeUnderlyingType::Int64)

        || (from == AttributeUnderlyingType::Int8 && to == AttributeUnderlyingType::Int16)
        || (from == AttributeUnderlyingType::Int8 && to == AttributeUnderlyingType::Int32)
        || (from == AttributeUnderlyingType::Int8 && to == AttributeUnderlyingType::Int64)
        || (from == AttributeUnderlyingType::Int16 && to == AttributeUnderlyingType::Int32)
        || (from == AttributeUnderlyingType::Int16 && to == AttributeUnderlyingType::Int64)
        || (from == AttributeUnderlyingType::Int32 && to == AttributeUnderlyingType::Int64)

        || (from == AttributeUnderlyingType::Float32 && to == AttributeUnderlyingType::Float64))
    {
        return true;
    }

    return false;
}


AttributeUnderlyingType getAttributeUnderlyingType(const std::string & type)
{
    static const std::unordered_map<std::string, AttributeUnderlyingType> dictionary{
        {"UInt8", AttributeUnderlyingType::UInt8},
        {"UInt16", AttributeUnderlyingType::UInt16},
        {"UInt32", AttributeUnderlyingType::UInt32},
        {"UInt64", AttributeUnderlyingType::UInt64},
        {"UUID", AttributeUnderlyingType::UInt128},
        {"Int8", AttributeUnderlyingType::Int8},
        {"Int16", AttributeUnderlyingType::Int16},
        {"Int32", AttributeUnderlyingType::Int32},
        {"Int64", AttributeUnderlyingType::Int64},
        {"Float32", AttributeUnderlyingType::Float32},
        {"Float64", AttributeUnderlyingType::Float64},
        {"String", AttributeUnderlyingType::String},
        {"Date", AttributeUnderlyingType::UInt16},
        {"DateTime", AttributeUnderlyingType::UInt32},
    };

    const auto it = dictionary.find(type);
    if (it != std::end(dictionary))
        return it->second;

    if (type.find("Decimal") == 0)
    {
        size_t start = strlen("Decimal");
        if (type.find("32", start) == start)
            return AttributeUnderlyingType::Decimal32;
        if (type.find("64", start) == start)
            return AttributeUnderlyingType::Decimal64;
        if (type.find("128", start) == start)
            return AttributeUnderlyingType::Decimal128;
    }

    throw Exception{"Unknown type " + type, ErrorCodes::UNKNOWN_TYPE};
}


std::string toString(const AttributeUnderlyingType type)
{
    switch (type)
    {
        case AttributeUnderlyingType::UInt8:
            return "UInt8";
        case AttributeUnderlyingType::UInt16:
            return "UInt16";
        case AttributeUnderlyingType::UInt32:
            return "UInt32";
        case AttributeUnderlyingType::UInt64:
            return "UInt64";
        case AttributeUnderlyingType::UInt128:
            return "UUID";
        case AttributeUnderlyingType::Int8:
            return "Int8";
        case AttributeUnderlyingType::Int16:
            return "Int16";
        case AttributeUnderlyingType::Int32:
            return "Int32";
        case AttributeUnderlyingType::Int64:
            return "Int64";
        case AttributeUnderlyingType::Float32:
            return "Float32";
        case AttributeUnderlyingType::Float64:
            return "Float64";
        case AttributeUnderlyingType::Decimal32:
            return "Decimal32";
        case AttributeUnderlyingType::Decimal64:
            return "Decimal64";
        case AttributeUnderlyingType::Decimal128:
            return "Decimal128";
        case AttributeUnderlyingType::String:
            return "String";
    }

    throw Exception{"Unknown attribute_type " + toString(static_cast<int>(type)), ErrorCodes::ARGUMENT_OUT_OF_BOUND};
}


DictionarySpecialAttribute::DictionarySpecialAttribute(const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix)
    : name{config.getString(config_prefix + ".name", "")}, expression{config.getString(config_prefix + ".expression", "")}
{
    if (name.empty() && !expression.empty())
        throw Exception{"Element " + config_prefix + ".name is empty", ErrorCodes::BAD_ARGUMENTS};
}


DictionaryStructure::DictionaryStructure(const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix)
{
    const auto has_id = config.has(config_prefix + ".id");
    const auto has_key = config.has(config_prefix + ".key");

    if (has_key && has_id)
        throw Exception{"Only one of 'id' and 'key' should be specified", ErrorCodes::BAD_ARGUMENTS};

    if (has_id)
        id.emplace(config, config_prefix + ".id");
    else if (has_key)
    {
        key.emplace(getAttributes(config, config_prefix + ".key", false, false));
        if (key->empty())
            throw Exception{"Empty 'key' supplied", ErrorCodes::BAD_ARGUMENTS};
    }
    else
        throw Exception{"Dictionary structure should specify either 'id' or 'key'", ErrorCodes::BAD_ARGUMENTS};

    if (id)
    {
        if (id->name.empty())
            throw Exception{"'id' cannot be empty", ErrorCodes::BAD_ARGUMENTS};

        const auto range_default_type = "Date";
        if (config.has(config_prefix + ".range_min"))
            range_min.emplace(makeDictionaryTypedSpecialAttribute(config, config_prefix + ".range_min", range_default_type));

        if (config.has(config_prefix + ".range_max"))
            range_max.emplace(makeDictionaryTypedSpecialAttribute(config, config_prefix + ".range_max", range_default_type));

        if (range_min.has_value() != range_max.has_value())
        {
            throw Exception{"Dictionary structure should have both 'range_min' and 'range_max' either specified or not.",
                            ErrorCodes::BAD_ARGUMENTS};
        }

        if (range_min && range_max && !range_min->type->equals(*range_max->type))
        {
            throw Exception{"Dictionary structure 'range_min' and 'range_max' should have same type, "
                            "'range_min' type: "
                                + range_min->type->getName()
                                + ", "
                                  "'range_max' type: "
                                + range_max->type->getName(),
                            ErrorCodes::BAD_ARGUMENTS};
        }

        if (range_min)
        {
            if (!range_min->type->isValueRepresentedByInteger())
                throw Exception{"Dictionary structure type of 'range_min' and 'range_max' should be an integer, Date, DateTime, or Enum."
                                " Actual 'range_min' and 'range_max' type is "
                                    + range_min->type->getName(),
                                ErrorCodes::BAD_ARGUMENTS};
        }

        if (!id->expression.empty() || (range_min && !range_min->expression.empty()) || (range_max && !range_max->expression.empty()))
            has_expressions = true;
    }

    attributes = getAttributes(config, config_prefix);
    if (attributes.empty())
        throw Exception{"Dictionary has no attributes defined", ErrorCodes::BAD_ARGUMENTS};
}


void DictionaryStructure::validateKeyTypes(const DataTypes & key_types) const
{
    if (key_types.size() != key->size())
        throw Exception{"Key structure does not match, expected " + getKeyDescription(), ErrorCodes::TYPE_MISMATCH};

    for (const auto i : ext::range(0, key_types.size()))
    {
        const auto & expected_type = (*key)[i].type->getName();
        const auto & actual_type = key_types[i]->getName();

        if (expected_type != actual_type)
            throw Exception{"Key type at position " + std::to_string(i) + " does not match, expected " + expected_type + ", found "
                                + actual_type,
                            ErrorCodes::TYPE_MISMATCH};
    }
}


std::string DictionaryStructure::getKeyDescription() const
{
    if (id)
        return "UInt64";

    std::ostringstream out;

    out << '(';

    auto first = true;
    for (const auto & key_i : *key)
    {
        if (!first)
            out << ", ";

        first = false;

        out << key_i.type->getName();
    }

    out << ')';

    return out.str();
}


bool DictionaryStructure::isKeySizeFixed() const
{
    if (!key)
        return true;

    for (const auto & key_i : *key)
        if (key_i.underlying_type == AttributeUnderlyingType::String)
            return false;

    return true;
}

size_t DictionaryStructure::getKeySize() const
{
    return std::accumulate(std::begin(*key), std::end(*key), size_t{}, [](const auto running_size, const auto & key_i)
    {
        return running_size + key_i.type->getSizeOfValueInMemory();
    });
}


static void checkAttributeKeys(const Poco::Util::AbstractConfiguration::Keys & keys)
{
    static const std::unordered_set<std::string> valid_keys
        = {"name", "type", "expression", "null_value", "hierarchical", "injective", "is_object_id"};

    for (const auto & key : keys)
    {
        if (valid_keys.find(key) == valid_keys.end())
            throw Exception{"Unknown key '" + key + "' inside attribute section", ErrorCodes::BAD_ARGUMENTS};
    }
}


std::vector<DictionaryAttribute> DictionaryStructure::getAttributes(
    const Poco::Util::AbstractConfiguration & config,
    const std::string & config_prefix,
    const bool hierarchy_allowed,
    const bool allow_null_values)
{
    Poco::Util::AbstractConfiguration::Keys config_elems;
    config.keys(config_prefix, config_elems);
    auto has_hierarchy = false;

    std::vector<DictionaryAttribute> res_attributes;

    const FormatSettings format_settings;

    for (const auto & config_elem : config_elems)
    {
        if (!startsWith(config_elem.data(), "attribute"))
            continue;

        const auto prefix = config_prefix + '.' + config_elem + '.';
        Poco::Util::AbstractConfiguration::Keys attribute_keys;
        config.keys(config_prefix + '.' + config_elem, attribute_keys);

        checkAttributeKeys(attribute_keys);

        const auto name = config.getString(prefix + "name");
        const auto type_string = config.getString(prefix + "type");
        const auto type = DataTypeFactory::instance().get(type_string);
        const auto underlying_type = getAttributeUnderlyingType(type_string);

        const auto expression = config.getString(prefix + "expression", "");
        if (!expression.empty())
            has_expressions = true;

        Field null_value;
        if (allow_null_values)
        {
            const auto null_value_string = config.getString(prefix + "null_value");
            try
            {
                if (null_value_string.empty())
                    null_value = type->getDefault();
                else
                {
                    ReadBufferFromString null_value_buffer{null_value_string};
                    auto column_with_null_value = type->createColumn();
                    type->deserializeAsTextEscaped(*column_with_null_value, null_value_buffer, format_settings);
                    null_value = (*column_with_null_value)[0];
                }
            }
            catch (Exception & e)
            {
                e.addMessage("error parsing null_value");
                throw;
            }
        }

        const auto hierarchical = config.getBool(prefix + "hierarchical", false);
        const auto injective = config.getBool(prefix + "injective", false);
        const auto is_object_id = config.getBool(prefix + "is_object_id", false);
        if (name.empty())
            throw Exception{"Properties 'name' and 'type' of an attribute cannot be empty", ErrorCodes::BAD_ARGUMENTS};

        if (has_hierarchy && !hierarchy_allowed)
            throw Exception{"Hierarchy not allowed in '" + prefix, ErrorCodes::BAD_ARGUMENTS};

        if (has_hierarchy && hierarchical)
            throw Exception{"Only one hierarchical attribute supported", ErrorCodes::BAD_ARGUMENTS};

        has_hierarchy = has_hierarchy || hierarchical;

        res_attributes.emplace_back(
            DictionaryAttribute{name, underlying_type, type, expression, null_value, hierarchical, injective, is_object_id});
    }

    return res_attributes;
}


void buildXMLRecursive(
    Poco::AutoPtr<Poco::XML::Document> doc,
    Poco::AutoPtr<Poco::XML::Element> root,
    const ASTKeyValueFunction * func)
{
    if (func == nullptr)
        return;

    Poco::AutoPtr<Poco::XML::Element> xml_element = doc->createElement(func->name);
    root->appendChild(xml_element);
    const ASTExpressionList * ast_expr_list = typeid_cast<const ASTExpressionList *>(func->children[0].get());
    for (size_t index = 0; index != ast_expr_list->children.size(); ++index)
    {
        const IAST * ast_element = ast_expr_list->children[index].get();
        if (ast_element->getID() == "pair")
        {
            const ASTPair * pair = typeid_cast<const ASTPair *>(ast_element);
            Poco::AutoPtr<Poco::XML::Element> current_xml_element = doc->createElement(pair->first);
            xml_element->appendChild(current_xml_element);
            const ASTLiteral * literal = typeid_cast<const ASTLiteral *>(pair->second.get());
            current_xml_element->appendChild(doc->createTextNode(literal->value.get<String>()));
        }
        else if (startsWith(ast_element->getID(), "KeyValueFunction"))
        {
            const ASTKeyValueFunction * current_func = typeid_cast<const ASTKeyValueFunction *>(ast_element);
            buildXMLRecursive(doc, xml_element, current_func);
        }
        else
            throw Exception("Source KeyValueFunction may contain only pair or another KeyValueFunction",
                    ErrorCodes::CANNOT_CONSTRUCT_CONFIGURATION_FROM_AST);
    }
}


void addSourceFieldsFromAST(
    Poco::AutoPtr<Poco::XML::Document> doc,
    Poco::AutoPtr<Poco::XML::Element> root,
    const ASTCreateQuery & create)
{
    if (create.dictionary_source == nullptr || create.dictionary_source->source == nullptr)
        return; // TODO: maybe it would be great throw an exception

    buildXMLRecursive(doc, root, create.dictionary_source->source);
}


void addLayoutFieldsFromAST(
    Poco::AutoPtr<Poco::XML::Document> doc,
    Poco::AutoPtr<Poco::XML::Element> root,
    const ASTCreateQuery & create)
{
    const auto * layout = create.dictionary_source->layout;
    if (layout == nullptr)
        throw Exception(std::string(__PRETTY_FUNCTION__) + ": layout is empty", ErrorCodes::BAD_ARGUMENTS);

    const auto * ast_expr_list = create.dictionary_source->layout->children[0].get();
    if (ast_expr_list->children.size() != 1)
        throw Exception(std::string(__PRETTY_FUNCTION__) + ": layout may contain only one parameter", ErrorCodes::BAD_ARGUMENTS);

    const auto & layout_type = typeid_cast<const ASTKeyValueFunction &>(*ast_expr_list->children[0].get());
    if (layout_type.children.size() > 1)
        throw Exception(std::string(__PRETTY_FUNCTION__) + ": layout type may contain only one parameter", ErrorCodes::BAD_ARGUMENTS);

    Poco::AutoPtr<Poco::XML::Element> layout_element = doc->createElement("layout");
    root->appendChild(layout_element);
    Poco::AutoPtr<Poco::XML::Element> layout_type_element = doc->createElement(layout_type.name);
    layout_element->appendChild(layout_type_element);

    const auto * layout_parameter_expr_list = layout_type.children.at(0).get();
    if (layout_parameter_expr_list->children.size() == 1)
    {
        const ASTPair & pair = typeid_cast<const ASTPair &>(*layout_parameter_expr_list->children[0].get());
        Poco::AutoPtr<Poco::XML::Element> layout_type_parameter_element = doc->createElement(pair.first);
        const ASTLiteral * literal = typeid_cast<const ASTLiteral *>(pair.second.get());
        layout_type_parameter_element->appendChild(doc->createTextNode(toString(literal->value.get<UInt64>())));
        layout_type_element->appendChild(layout_type_parameter_element);
    }
}


void addLifetimeFieldsFromAST(
    Poco::AutoPtr<Poco::XML::Document> doc,
    Poco::AutoPtr<Poco::XML::Element> root,
    const ASTCreateQuery & create)
{
    auto lifetime = ExternalLoadableLifetime(create.dictionary_source->lifetime);
    Poco::AutoPtr<Poco::XML::Element> lifetime_element = doc->createElement("lifetime");
    Poco::AutoPtr<Poco::XML::Element> min_element = doc->createElement("min");
    Poco::AutoPtr<Poco::XML::Element> max_element = doc->createElement("max");
    min_element->appendChild(doc->createTextNode(toString(lifetime.min_sec)));
    max_element->appendChild(doc->createTextNode(toString(lifetime.max_sec)));
    lifetime_element->appendChild(min_element);
    lifetime_element->appendChild(max_element);
    root->appendChild(lifetime_element);
}


void addAdditionalColumnFields(
    Poco::AutoPtr<Poco::XML::Document> doc,
    Poco::AutoPtr<Poco::XML::Element> root,
    const ASTColumnDeclaration * column_declaration)
{
    const ASTExpressionList * expr_list = typeid_cast<const ASTExpressionList *>(column_declaration->expr_list.get());
    for (size_t index = 0; index != expr_list->children.size(); ++index)
    {
        const auto * child = expr_list->children.at(index).get();
        const ASTPair * pair = typeid_cast<const ASTPair *>(child);
        auto pair_element = doc->createElement(pair->first);
        pair_element->appendChild(doc->createTextNode(queryToString(pair->second)));
        root->appendChild(pair_element);
    }
}


void addRangeFieldsFromAST(
    Poco::AutoPtr<Poco::XML::Document> doc,
    Poco::AutoPtr<Poco::XML::Element> root,
    const ASTKeyValueFunction * range)
{
    const ASTExpressionList * expr_list = typeid_cast<const ASTExpressionList *>(range->children.at(0).get());
    if (expr_list->children.size() != 2)
        throw Exception("Number of arguments of RANGE() other than 2", ErrorCodes::CANNOT_CONSTRUCT_CONFIGURATION_FROM_AST);

    for (size_t index = 0; index != expr_list->children.size(); ++index)
    {
        const auto * child = expr_list->children.at(index).get();
        const ASTPair * pair = typeid_cast<const ASTPair *>(child);
        Poco::AutoPtr<Poco::XML::Element> name = doc->createElement("name");
        name->appendChild(doc->createTextNode(queryToString(pair->second)));

        if (pair->first == "min")
        {
            Poco::AutoPtr<Poco::XML::Element> range_min_element = doc->createElement("range_min");
            range_min_element->appendChild(name);
            root->appendChild(range_min_element);
        }
        else if (pair->first == "max")
        {
            Poco::AutoPtr<Poco::XML::Element> range_max_element = doc->createElement("range_max");
            range_max_element->appendChild(name);
            root->appendChild(range_max_element);
        }
        else
            throw Exception("Key of argument should be either MIN or MAX", ErrorCodes::CANNOT_CONSTRUCT_CONFIGURATION_FROM_AST);
    }
}


void addStructureFieldsFromAST(
    Poco::AutoPtr<Poco::XML::Document> doc,
    Poco::AutoPtr<Poco::XML::Element> root,
    const ASTCreateQuery & create)
{
    if (create.dictionary_source == nullptr)
        throw Exception("Can't construct configuration without dictionary structure", ErrorCodes::CANNOT_CONSTRUCT_CONFIGURATION_FROM_AST);

    const auto * source = create.dictionary_source;
    Poco::AutoPtr<Poco::XML::Element> structure_element = doc->createElement("structure");
    root->appendChild(structure_element);
    if (source->primary_key)
    {
        const ASTExpressionList * expr_list = typeid_cast<const ASTExpressionList *>(source->primary_key);
        if (expr_list->children.size() != 1)
            throw Exception("Primary key may be only one column", ErrorCodes::CANNOT_CONSTRUCT_CONFIGURATION_FROM_AST); // TODO: this is wrong because of complex key

        // TODO: support complex key here later
        auto column_name = expr_list->children[0]->getColumnName();
        Poco::AutoPtr<Poco::XML::Element> id_element = doc->createElement("id");
        structure_element->appendChild(id_element);
        Poco::AutoPtr<Poco::XML::Element> name_element = doc->createElement("name");
        id_element->appendChild(name_element);
        name_element->appendChild(doc->createTextNode(column_name));
    }

    if (create.columns_list == nullptr)
        throw Exception("Can't construct configuration without columns declaration", ErrorCodes::CANNOT_CONSTRUCT_CONFIGURATION_FROM_AST);

    if (source->range)
        addRangeFieldsFromAST(doc, structure_element, source->range);

    const ASTExpressionList * columns = create.columns_list->columns;
    for (size_t index = 0; index != columns->children.size(); ++index)
    {
        const auto * child = columns->children[index].get();
        const ASTColumnDeclaration * column_declaration = typeid_cast<const ASTColumnDeclaration *>(child);

        if (!column_declaration->type || !column_declaration->default_expression)
            throw Exception("Column declaration of dictionary should contain type and default expression", ErrorCodes::BAD_ARGUMENTS);

        Poco::AutoPtr<Poco::XML::Element> attribute_element = doc->createElement("attribute");
        structure_element->appendChild(attribute_element);
        Poco::AutoPtr<Poco::XML::Element> name_element = doc->createElement("name");
        name_element->appendChild(doc->createTextNode(column_declaration->name));
        attribute_element->appendChild(name_element);

        // TODO: it would be great to check type
        auto type = typeid_cast<const ASTFunction *>(column_declaration->type.get())->name;
        Poco::AutoPtr<Poco::XML::Element> type_element = doc->createElement("type");
        type_element->appendChild(doc->createTextNode(type));
        attribute_element->appendChild(type_element);

        Poco::AutoPtr<Poco::XML::Element> null_value_element = doc->createElement("null_value");
        null_value_element->appendChild(doc->createTextNode(queryToString(column_declaration->default_expression)));
        attribute_element->appendChild(null_value_element);

        addAdditionalColumnFields(doc, attribute_element, column_declaration);
    }
}

Poco::AutoPtr<Poco::Util::AbstractConfiguration> getDictionaryConfigFromAST(const ASTCreateQuery & create)
{
    Poco::AutoPtr<Poco::XML::Document> xml_document = new Poco::XML::Document();
    Poco::AutoPtr<Poco::XML::Element> document_root = xml_document->createElement("dictionaries");
    xml_document->appendChild(document_root);
    Poco::AutoPtr<Poco::XML::Element> current_dictionary = xml_document->createElement("dictionary");
    document_root->appendChild(current_dictionary);
    Poco::AutoPtr<Poco::Util::XMLConfiguration> conf = new Poco::Util::XMLConfiguration();
    if (create.dictionary.empty())
        return conf;

    Poco::AutoPtr<Poco::XML::Element> name_element = xml_document->createElement("name");
    name_element->appendChild(xml_document->createTextNode("create.dictionary"));
    current_dictionary->appendChild(name_element);

    addSourceFieldsFromAST(xml_document, current_dictionary, create);
    addLayoutFieldsFromAST(xml_document, current_dictionary, create);
    addStructureFieldsFromAST(xml_document, current_dictionary, create);
    addLifetimeFieldsFromAST(xml_document, current_dictionary, create);

    conf->load(xml_document);
    return conf;
}

}
