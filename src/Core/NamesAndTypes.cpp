#include <Core/NamesAndTypes.h>

#include <base/sort.h>
#include <Common/HashTable/HashMap.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/IDataType.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <IO/Operators.h>

#include <boost/algorithm/string.hpp>
#include <cstddef>

namespace DB
{

namespace ErrorCodes
{
    extern const int THERE_IS_NO_COLUMN;
}

NameAndTypePair::NameAndTypePair(
    const String & name_in_storage_, const String & subcolumn_name_,
    const DataTypePtr & type_in_storage_, const DataTypePtr & subcolumn_type_)
    : name(name_in_storage_ + (subcolumn_name_.empty() ? "" : "." + subcolumn_name_))
    , type(subcolumn_type_)
    , type_in_storage(type_in_storage_)
    , subcolumn_delimiter_position(subcolumn_name_.empty() ? std::nullopt : std::make_optional(name_in_storage_.size()))
{
}

String NameAndTypePair::getNameInStorage() const
{
    if (!subcolumn_delimiter_position)
        return name;

    return name.substr(0, *subcolumn_delimiter_position);
}

bool NameAndTypePair::operator<(const NameAndTypePair & rhs) const
{
    return std::forward_as_tuple(name, type->getName()) < std::forward_as_tuple(rhs.name, rhs.type->getName());
}

bool NameAndTypePair::operator==(const NameAndTypePair & rhs) const
{
    return name == rhs.name && type->equals(*rhs.type);
}

String NameAndTypePair::getSubcolumnName() const
{
    if (!subcolumn_delimiter_position)
        return "";

    return name.substr(*subcolumn_delimiter_position + 1, name.size() - *subcolumn_delimiter_position);
}

String NameAndTypePair::dump() const
{
    WriteBufferFromOwnString out;
    out << "name: " << name << "\n"
        << "type: " << type->getName() << "\n"
        << "name in storage: " << getNameInStorage() << "\n"
        << "type in storage: " << getTypeInStorage()->getName();

    return out.str();
}

void NamesAndTypesList::readText(ReadBuffer & buf, bool check_eof)
{
    const DataTypeFactory & data_type_factory = DataTypeFactory::instance();

    assertString("columns format version: 1\n", buf);
    size_t count;
    DB::readText(count, buf);
    assertString(" columns:\n", buf);

    String column_name;
    String type_name;
    for (size_t i = 0; i < count; ++i)
    {
        readBackQuotedStringWithSQLStyle(column_name, buf);
        assertChar(' ', buf);
        readString(type_name, buf);
        assertChar('\n', buf);

        emplace_back(column_name, data_type_factory.get(type_name));
    }

    if (check_eof)
        assertEOF(buf);
}

void NamesAndTypesList::writeText(WriteBuffer & buf) const
{
    writeString("columns format version: 1\n", buf);
    DB::writeText(size(), buf);
    writeString(" columns:\n", buf);
    for (const auto & it : *this)
    {
        writeBackQuotedString(it.name, buf);
        writeChar(' ', buf);
        writeString(it.type->getName(), buf);
        writeChar('\n', buf);
    }
}

String NamesAndTypesList::toString() const
{
    WriteBufferFromOwnString out;
    writeText(out);
    return out.str();
}

NamesAndTypesList NamesAndTypesList::parse(const String & s)
{
    ReadBufferFromString in(s);
    NamesAndTypesList res;
    res.readText(in);
    assertEOF(in);
    return res;
}

bool NamesAndTypesList::isSubsetOf(const NamesAndTypesList & rhs) const
{
    NamesAndTypes vector(rhs.begin(), rhs.end());
    vector.insert(vector.end(), begin(), end());
    ::sort(vector.begin(), vector.end());
    return std::unique(vector.begin(), vector.end()) == vector.begin() + rhs.size();
}

size_t NamesAndTypesList::sizeOfDifference(const NamesAndTypesList & rhs) const
{
    NamesAndTypes vector(rhs.begin(), rhs.end());
    vector.insert(vector.end(), begin(), end());
    ::sort(vector.begin(), vector.end());
    return (std::unique(vector.begin(), vector.end()) - vector.begin()) * 2 - size() - rhs.size();
}

void NamesAndTypesList::getDifference(const NamesAndTypesList & rhs, NamesAndTypesList & deleted, NamesAndTypesList & added) const
{
    NamesAndTypes lhs_vector(begin(), end());
    ::sort(lhs_vector.begin(), lhs_vector.end());
    NamesAndTypes rhs_vector(rhs.begin(), rhs.end());
    ::sort(rhs_vector.begin(), rhs_vector.end());

    std::set_difference(lhs_vector.begin(), lhs_vector.end(), rhs_vector.begin(), rhs_vector.end(),
        std::back_inserter(deleted));
    std::set_difference(rhs_vector.begin(), rhs_vector.end(), lhs_vector.begin(), lhs_vector.end(),
        std::back_inserter(added));
}

Names NamesAndTypesList::getNames() const
{
    Names res;
    res.reserve(size());
    for (const NameAndTypePair & column : *this)
        res.push_back(column.name);
    return res;
}

NameSet NamesAndTypesList::getNameSet() const
{
    NameSet res;
    res.reserve(size());
    for (const NameAndTypePair & column : *this)
        res.insert(column.name);
    return res;
}

std::unordered_map<std::string, DataTypePtr> NamesAndTypesList::getNameToTypeMap() const
{
    std::unordered_map<std::string, DataTypePtr> res;
    res.reserve(size());
    for (const NameAndTypePair & column : *this)
        res.emplace(column.name, column.type);
    return res;
}

DataTypes NamesAndTypesList::getTypes() const
{
    DataTypes res;
    res.reserve(size());
    for (const NameAndTypePair & column : *this)
        res.push_back(column.type);
    return res;
}

void NamesAndTypesList::filterColumns(const NameSet & names)
{
    for (auto it = begin(); it != end();)
    {
        const auto & column = *it;
        if (names.contains(column.name))
            ++it;
        else
            it = erase(it);
    }
}

NamesAndTypesList NamesAndTypesList::filter(const NameSet & names) const
{
    NamesAndTypesList res;
    for (const NameAndTypePair & column : *this)
    {
        if (names.contains(column.name))
            res.push_back(column);
    }
    return res;
}

NamesAndTypesList NamesAndTypesList::filter(const Names & names) const
{
    return filter(NameSet(names.begin(), names.end()));
}

NamesAndTypesList NamesAndTypesList::eraseNames(const NameSet & names) const
{
    NamesAndTypesList res;
    for (const auto & column : *this)
    {
        if (!names.contains(column.name))
            res.push_back(column);
    }
    return res;
}


NamesAndTypesList NamesAndTypesList::addTypes(const Names & names) const
{
    /// NOTE: It's better to make a map in `IStorage` than to create it here every time again.
    HashMapWithSavedHash<StringRef, const DataTypePtr *, StringRefHash> types;

    for (const auto & column : *this)
        types[column.name] = &column.type;

    NamesAndTypesList res;
    for (const String & name : names)
    {
        const auto * it = types.find(name);
        if (it == types.end())
            throw Exception(ErrorCodes::THERE_IS_NO_COLUMN, "No column {}", name);

        res.emplace_back(name, *it->getMapped());
    }

    return res;
}

bool NamesAndTypesList::contains(const String & name) const
{
    for (const NameAndTypePair & column : *this)
    {
        if (column.name == name)
            return true;
    }
    return false;
}

bool NamesAndTypesList::containsCaseInsensitive(const String & name) const
{
    for (const NameAndTypePair & column : *this)
    {
        if (boost::iequals(column.name, name))
            return true;
    }
    return false;
}

std::optional<NameAndTypePair> NamesAndTypesList::tryGetByName(const std::string & name) const
{
    for (const NameAndTypePair & column : *this)
    {
        if (column.name == name)
            return column;
    }
    return {};
}

size_t NamesAndTypesList::getPosByName(const std::string &name) const noexcept
{
    size_t pos = 0;
    for (const NameAndTypePair & column : *this)
    {
        if (column.name == name)
            break;
        ++pos;
    }
    return pos;
}

String NamesAndTypesList::toNamesAndTypesDescription() const
{
    WriteBufferFromOwnString buf;
    bool first = true;
    for (const auto & name_and_type : *this)
    {
        if (!std::exchange(first, false))
            writeCString(", ", buf);
        writeBackQuotedString(name_and_type.name, buf);
        writeChar(' ', buf);
        writeString(name_and_type.type->getName(), buf);
    }
    return buf.str();
}

void NamesAndTypesList::readTextWithNamesInStorage(ReadBuffer & buf)
{
    const DataTypeFactory & data_type_factory = DataTypeFactory::instance();

    assertString("columns format version: 1\n", buf);
    size_t count;
    DB::readText(count, buf);
    assertString(" columns:\n", buf);

    String column_name;
    String type_name;
    String name_in_storage;
    String type_in_storage;
    for (size_t i = 0; i < count; ++i)
    {
        buf >> "name: ";
        readBackQuotedStringWithSQLStyle(column_name, buf);
        buf >> "\n";

        buf >> "type: " >> type_name >> "\n";

        buf >> "name in storage: ";
        readBackQuotedStringWithSQLStyle(name_in_storage, buf);
        buf >> "\n";

        buf >> "type in storage: " >> type_in_storage >> "\n";

        emplace_back(
            column_name,
            name_in_storage,
            data_type_factory.get(type_in_storage),
            data_type_factory.get(type_name));
    }

}

void NamesAndTypesList::writeTextWithNamesInStorage(WriteBuffer & buf) const
{
    writeString("columns format version: 1\n", buf);
    DB::writeText(size(), buf);
    writeString(" columns:\n", buf);
    for (const auto & it : *this)
    {
        buf << "name: ";
        writeBackQuotedString(it.getNameInStorage(), buf);
        buf << "\n";

        buf << "type: " << it.type->getName() << "\n";

        buf << "name in storage: ";
        writeBackQuotedString(it.getSubcolumnName(), buf);
        buf << "\n";

        buf << "type in storage: " << it.getTypeInStorage()->getName() << "\n";
    }
}

}
