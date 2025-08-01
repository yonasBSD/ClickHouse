#pragma once

#include <base/types.h>
#include <Core/Names.h>
#include <Interpreters/StorageID.h>

#include <map>


namespace Poco
{
namespace Util
{
    class AbstractConfiguration;
}
class Logger;
}


namespace DB
{

using LoggerPtr = std::shared_ptr<Poco::Logger>;

/** Apply substitutions from the macros in config to the string.
  */
class Macros
{
public:
    Macros() = default;
    Macros(const Poco::Util::AbstractConfiguration & config, const String & key, LoggerPtr log = nullptr);
    Macros(const Poco::Util::AbstractConfiguration & config, const String & key, Poco::Logger * log = nullptr);
    explicit Macros(std::map<String, String> map);

    struct MacroExpansionInfo
    {
        /// Settings
        StorageID table_id = StorageID::createEmpty();
        bool ignore_unknown = false;
        bool expand_special_macros_only = false;
        std::optional<String> shard = {};
        std::optional<String> replica = {};

        /// Information about macro expansion
        size_t level = 0;
        bool expanded_database = false;
        bool expanded_table = false;
        bool expanded_uuid = false;
        bool expanded_other = false;
        bool has_unknown = false;
    };

    /** Replace the substring of the form {macro_name} with the value for macro_name, obtained from the config file.
      * If {database} and {table} macros aren`t defined explicitly, expand them as database_name and table_name respectively.
      * level - the level of recursion.
      */
    String expand(const String & s,
                  MacroExpansionInfo & info) const;

    String expand(const String & s) const;


    /** Apply expand for the list.
      */
    Names expand(const Names & source_names, size_t level = 0) const;

    using MacroMap = std::map<String, String>;
    MacroMap getMacroMap() const { return macros; }

    String getValue(const String & key) const;
    std::optional<String> tryGetValue(const String & key) const;

private:
    MacroMap macros;
    bool enable_special_macros = true;
};


}
