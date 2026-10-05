#ifndef GAME_EDITOR_MCP_PROTOCOL_H
#define GAME_EDITOR_MCP_PROTOCOL_H

#include "mcp_server.h"

#include <engine/shared/json.h>

namespace EditorMcp
{
	std::string Quote(const std::string &Text);
	std::string Encode(const json_value &Value);
	std::string Rpc(const std::string &Id, const std::string &Result);
	std::string TimeoutResponse(const std::string &RequestBody);
	std::string TextResult(const std::string &Json);
	using FToolHandler = std::function<bool(const std::string &Name, const json_value &Arguments, const std::string &Id, CEditorMcpServer::CRequest &Request)>;
	bool Dispatch(CEditorMcpServer::CRequest &Request, const FToolHandler &ToolHandler);
}

#endif
