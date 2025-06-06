#include <Core/Names.h>
#include <Core/Settings.h>
#include <Interpreters/QueryNormalizer.h>
#include <Interpreters/IdentifierSemantic.h>
#include <Interpreters/Context.h>
#include <Interpreters/RequiredSourceColumnsVisitor.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTQueryParameter.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTInterpolateElement.h>
#include <Common/quoteString.h>

namespace DB
{

namespace Setting
{
extern const SettingsUInt64 max_ast_depth;
extern const SettingsUInt64 max_expanded_ast_elements;
extern const SettingsBool prefer_column_name_to_alias;
}

namespace ErrorCodes
{
    extern const int TOO_DEEP_AST;
    extern const int CYCLIC_ALIASES;
    extern const int UNKNOWN_QUERY_PARAMETER;
    extern const int BAD_ARGUMENTS;
}


QueryNormalizer::ExtractedSettings::ExtractedSettings(const Settings & settings)
    : max_ast_depth(settings[Setting::max_ast_depth])
    , max_expanded_ast_elements(settings[Setting::max_expanded_ast_elements])
    , prefer_column_name_to_alias(settings[Setting::prefer_column_name_to_alias])
{
}

class CheckASTDepth
{
public:
    explicit CheckASTDepth(QueryNormalizer::Data & data_)
        : data(data_)
    {
        if (data.level > data.settings.max_ast_depth)
            throw Exception(ErrorCodes::TOO_DEEP_AST, "Normalized AST is too deep. Maximum: {}", data.settings.max_ast_depth);
        ++data.level;
    }

    ~CheckASTDepth()
    {
        --data.level;
    }

private:
    QueryNormalizer::Data & data;
};


class RestoreAliasOnExitScope
{
public:
    explicit RestoreAliasOnExitScope(String & alias_)
        : alias(alias_)
        , copy(alias_)
    {}

    ~RestoreAliasOnExitScope()
    {
        alias = copy;
    }

private:
    String & alias;
    const String copy;
};


void QueryNormalizer::visit(ASTIdentifier & node, ASTPtr & ast, Data & data)
{
    /// We do handle cycles via tracking current_asts
    /// but in case of bug in that tricky logic we need to prevent stack overflow
    checkStackSize();

    auto & current_asts = data.current_asts;
    String & current_alias = data.current_alias;

    if (!IdentifierSemantic::getColumnName(node))
        return;

    if (data.settings.prefer_column_name_to_alias)
    {
        if (data.source_columns_set.find(node.name()) != data.source_columns_set.end())
            return;
    }

    /// If it is an alias, but not a parent alias (for constructs like "SELECT column + 1 AS column").
    if (!data.allow_self_aliases && current_alias == node.name())
        throw Exception(ErrorCodes::CYCLIC_ALIASES, "Self referencing of {} to {}. Cyclic alias",
                        backQuote(current_alias), backQuote(node.name()));
    auto it_alias = data.aliases.find(node.name());

    if (it_alias != data.aliases.end() && current_alias != node.name())
    {
        if (!IdentifierSemantic::canBeAlias(node))
            return;

        /// We are alias for other column (node.name), but we are alias by
        /// ourselves to some other column
        const auto & alias_node = it_alias->second;

        String our_alias_or_name = alias_node->getAliasOrColumnName();
        std::optional<String> our_name = IdentifierSemantic::getColumnName(alias_node);

        String node_alias = ast->tryGetAlias();

        if (current_asts.contains(alias_node.get()) /// We have loop of multiple aliases
            || (node.name() == our_alias_or_name && our_name && node_alias == *our_name)) /// Our alias points to node.name, direct loop
            throw Exception(ErrorCodes::CYCLIC_ALIASES, "Cyclic aliases");

        /// Let's replace it with the corresponding tree node.
        if (!node_alias.empty() && node_alias != our_alias_or_name)
        {
            /// Avoid infinite recursion here
            auto opt_name = IdentifierSemantic::getColumnName(alias_node);
            bool is_cycle = opt_name && *opt_name == node.name();

            if (!is_cycle)
            {
                /// In a construct like "a AS b", where a is an alias, you must set alias b to the result of substituting alias a.
                /// Check size of the alias before cloning too large alias AST
                alias_node->checkSize(data.settings.max_expanded_ast_elements);
                ast = alias_node->clone();
                ast->setAlias(node_alias);

                /// If the cloned AST was finished, this one should also be considered finished
                if (data.finished_asts.contains(alias_node))
                    data.finished_asts[ast] = ast;

                /// If we had an alias for node_alias, point it instead to the new node so we don't have to revisit it
                /// on subsequent calls
                if (auto existing_alias = data.aliases.find(node_alias); existing_alias != data.aliases.end())
                    existing_alias->second = ast;
            }
        }
        else
        {
            /// Check size of the alias before cloning too large alias AST
            alias_node->checkSize(data.settings.max_expanded_ast_elements);
            auto alias_name = ast->getAliasOrColumnName();
            ast = alias_node->clone();
            ast->setAlias(alias_name);

            /// If the cloned AST was finished, this one should also be considered finished
            if (data.finished_asts.contains(alias_node))
                data.finished_asts[ast] = ast;

            /// If we had an alias for node_alias, point it instead to the new node so we don't have to revisit it
            /// on subsequent calls
            if (auto existing_alias = data.aliases.find(node_alias); existing_alias != data.aliases.end())
                existing_alias->second = ast;
        }
    }
}


void QueryNormalizer::visit(ASTTablesInSelectQueryElement & node, const ASTPtr &, Data & data)
{
    /// normalize JOIN ON section
    if (node.table_join)
    {
        auto & join = node.table_join->as<ASTTableJoin &>();
        if (join.on_expression)
        {
            ASTPtr original_on_expression = join.on_expression;
            visit(join.on_expression, data);
            if (join.on_expression != original_on_expression)
                join.children = { join.on_expression };
        }

    }
}

static bool needVisitChild(const ASTPtr & child)
{
    /// exclude interpolate elements - they are not subject for normalization and will be processed in filling transform
    return !(child->as<ASTSelectQuery>() || child->as<ASTTableExpression>() || child->as<ASTInterpolateElement>());
}

/// special visitChildren() for ASTSelectQuery
void QueryNormalizer::visit(ASTSelectQuery & select, const ASTPtr &, Data & data)
{
    for (auto & child : select.children)
    {
        if (needVisitChild(child))
            visit(child, data);
    }

    /// If the WHERE clause or HAVING consists of a single alias, the reference must be replaced not only in children,
    /// but also in where_expression and having_expression.
    if (select.prewhere())
        visit(select.refPrewhere(), data);
    if (select.where())
        visit(select.refWhere(), data);
    if (select.having())
        visit(select.refHaving(), data);
}

/// Don't go into subqueries.
/// Don't go into select query. It processes children itself.
/// Do not go to the left argument of lambda expressions, so as not to replace the formal parameters
///  on aliases in expressions of the form 123 AS x, arrayMap(x -> 1, [2]).
void QueryNormalizer::visitChildren(IAST * node, Data & data)
{
    if (auto * func_node = node->as<ASTFunction>())
    {
        if (func_node->tryGetQueryArgument())
        {
            if (func_node->name != "view")
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Query argument can only be used in the `view` TableFunction");
            /// Don't go into query argument.
            return;
        }

        /// For lambda functions we need to avoid replacing lambda parameters with external aliases, for example,
        /// Select 1 as x, arrayMap(x -> x + 2, [1, 2, 3])
        /// shouldn't be replaced with Select 1 as x, arrayMap(x -> **(1 as x)** + 2, [1, 2, 3])
        Aliases extracted_aliases;
        if (func_node->name == "lambda")
        {
            Names lambda_aliases = RequiredSourceColumnsMatcher::extractNamesFromLambda(*func_node);
            for (const auto & name : lambda_aliases)
            {
                auto it = data.aliases.find(name);
                if (it != data.aliases.end())
                {
                    extracted_aliases.insert(data.aliases.extract(it));
                }
            }
        }

        /// We skip the first argument. We also assume that the lambda function can not have parameters.
        size_t first_pos = 0;
        if (func_node->name == "lambda")
            first_pos = 1;

        if (func_node->arguments)
        {
            auto & func_children = func_node->arguments->children;

            for (size_t i = first_pos; i < func_children.size(); ++i)
            {
                auto & child = func_children[i];

                if (needVisitChild(child))
                    visit(child, data);
            }
        }

        if (func_node->window_definition)
        {
            visitChildren(func_node->window_definition.get(), data);
        }

        for (auto & it : extracted_aliases)
        {
            data.aliases.insert(it);
        }
    }
    else if (!node->as<ASTSelectQuery>())
    {
        for (auto & child : node->children)
            if (needVisitChild(child))
                visit(child, data);
    }
}

void QueryNormalizer::visit(ASTPtr & ast, Data & data)
{
    CheckASTDepth scope1(data);
    RestoreAliasOnExitScope scope2(data.current_alias);

    auto & finished_asts = data.finished_asts;
    auto & current_asts = data.current_asts;

    if (finished_asts.contains(ast))
    {
        ast = finished_asts[ast];
        return;
    }

    ASTPtr initial_ast = ast;
    current_asts.insert(initial_ast.get());

    {
        String my_alias = ast->tryGetAlias();
        if (!my_alias.empty())
            data.current_alias = my_alias;
    }

    if (auto * node_id = ast->as<ASTIdentifier>())
        visit(*node_id, ast, data);
    else if (auto * node_tables = ast->as<ASTTablesInSelectQueryElement>())
        visit(*node_tables, ast, data);
    else if (auto * node_select = ast->as<ASTSelectQuery>())
        visit(*node_select, ast, data);
    else if (auto * node_param = ast->as<ASTQueryParameter>())
    {
        if (!data.is_create_parameterized_view)
            throw Exception(ErrorCodes::UNKNOWN_QUERY_PARAMETER, "Query parameter {} was not set", backQuote(node_param->name));
    }
    else if (auto * node_function = ast->as<ASTFunction>())
        if (node_function->parameters)
            visit(node_function->parameters, data);

    /// If we replace the root of the subtree, we will be called again for the new root, in case the alias is replaced by an alias.
    if (ast.get() != initial_ast.get())
        visit(ast, data);
    else
        visitChildren(ast.get(), data);

    current_asts.erase(initial_ast.get());
    current_asts.erase(ast.get());
    if (data.ignore_alias && !ast->tryGetAlias().empty())
        ast->setAlias("");
    finished_asts[initial_ast] = ast;

    /// @note can not place it in CheckASTDepth dtor cause of exception.
    if (data.level == 1)
    {
        try
        {
            ast->checkSize(data.settings.max_expanded_ast_elements);
        }
        catch (Exception & e)
        {
            e.addMessage("(after expansion of aliases)");
            throw;
        }
    }
}

}
