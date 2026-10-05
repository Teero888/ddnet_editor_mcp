#ifndef GAME_EDITOR_MCP_TOOLS_H
#define GAME_EDITOR_MCP_TOOLS_H

#include <string>

namespace EditorMcp
{
	// Result of tools/list: {"tools":[...]} with every tool's parameter schema.
	const std::string &ToolCatalog();
	// Result of editor_operations: operation descriptions, schemas and conventions.
	const std::string &OperationCatalog();
}

#endif
