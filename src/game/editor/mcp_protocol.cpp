#include "mcp_protocol.h"

#include "mcp_tools.h"

#include <base/str.h>

#include <engine/shared/jsonwriter.h>

#include <iomanip>
#include <locale>
#include <sstream>

namespace EditorMcp
{
	using PJson = std::unique_ptr<json_value, decltype(&json_value_free)>;
	static PJson Parse(const std::string &Text)
	{
		return PJson(JsonParse(Text.c_str(), Text.size()), json_value_free);
	}
	static const char *String(const json_value *pValue)
	{
		return pValue->type == json_string ? pValue->u.string.ptr : "";
	}
	std::string Quote(const std::string &Text)
	{
		CJsonStringWriter Writer;
		Writer.WriteStrValue(Text.c_str());
		return Writer.GetOutputString();
	}
	std::string Encode(const json_value &Value)
	{
		switch(Value.type)
		{
		case json_string: return Quote(Value.u.string.ptr);
		case json_integer: return std::to_string(Value.u.integer);
		case json_boolean: return Value.u.boolean ? "true" : "false";
		case json_double:
		{
			std::ostringstream Stream;
			Stream.imbue(std::locale::classic());
			Stream << std::setprecision(17) << Value.u.dbl;
			return Stream.str();
		}
		case json_object:
		{
			std::string Result = "{";
			for(unsigned i = 0; i < Value.u.object.length; ++i)
			{
				if(i)
					Result += ',';
				Result += Quote(Value.u.object.values[i].name) + ":" + Encode(*Value.u.object.values[i].value);
			}
			return Result + "}";
		}
		case json_array:
		{
			std::string Result = "[";
			for(unsigned i = 0; i < Value.u.array.length; ++i)
			{
				if(i)
					Result += ',';
				Result += Encode(Value[i]);
			}
			return Result + "]";
		}
		default: return "null";
		}
	}
	bool Validate(const json_value &Value, const json_value &Schema)
	{
		if(Schema["oneOf"].type == json_array)
		{
			const auto &Choices = Schema["oneOf"];
			for(unsigned i = 0; i < Choices.u.array.length; ++i)
				if(Validate(Value, Choices[i]))
					return true;
			return false;
		}
		if(Schema["const"].type != json_none && Encode(Value) != Encode(Schema["const"]))
			return false;
		const auto &Enum = Schema["enum"];
		if(Enum.type == json_array)
		{
			bool Match = false;
			for(unsigned i = 0; i < Enum.u.array.length; ++i)
				Match |= Encode(Value) == Encode(Enum[i]);
			if(!Match)
				return false;
		}
		const std::string Type = String(&Schema["type"]);
		if(Type == "integer")
			return Value.type == json_integer && (Schema["minimum"].type == json_none || Value.u.integer >= Schema["minimum"].u.integer) && (Schema["maximum"].type == json_none || Value.u.integer <= Schema["maximum"].u.integer);
		if(Type == "boolean")
			return Value.type == json_boolean;
		if(Type == "string")
			return Value.type == json_string && str_length(Value.u.string.ptr) == (int)Value.u.string.length && (Schema["maxLength"].type == json_none || Value.u.string.length <= Schema["maxLength"].u.integer);
		if(Type == "array")
		{
			if(Value.type != json_array || (Schema["minItems"].type != json_none && Value.u.array.length < Schema["minItems"].u.integer) || (Schema["maxItems"].type != json_none && Value.u.array.length > Schema["maxItems"].u.integer))
				return false;
			for(unsigned i = 0; i < Value.u.array.length; ++i)
				if(!Validate(Value[i], Schema["items"]))
					return false;
			return true;
		}
		if(Type == "object")
		{
			if(Value.type != json_object)
				return false;
			const auto &Required = Schema["required"];
			if(Required.type == json_array)
				for(unsigned i = 0; i < Required.u.array.length; ++i)
					if(Value[String(&Required[i])].type == json_none)
						return false;
			for(unsigned i = 0; i < Value.u.object.length; ++i)
			{
				const auto &Entry = Value.u.object.values[i];
				const auto &Property = Schema["properties"][static_cast<const char *>(Entry.name)];
				if(Property.type == json_none)
				{
					if(Schema["additionalProperties"].type == json_boolean && !Schema["additionalProperties"].u.boolean)
						return false;
				}
				else if(!Validate(*Entry.value, Property))
					return false;
			}
		}
		return true;
	}
	std::string Rpc(const std::string &Id, const std::string &Result)
	{
		return "{\"jsonrpc\":\"2.0\",\"id\":" + Id + ",\"result\":" + Result + "}";
	}
	std::string RpcError(const std::string &Id, int Code, const char *pMessage)
	{
		return "{\"jsonrpc\":\"2.0\",\"id\":" + Id + ",\"error\":{\"code\":" + std::to_string(Code) + ",\"message\":" + Quote(pMessage) + "}}";
	}
	std::string TimeoutResponse(const std::string &RequestBody)
	{
		auto Message = Parse(RequestBody);
		std::string Id = "null";
		if(Message && Message->type == json_object)
		{
			const auto &Value = (*Message)["id"];
			if(Value.type == json_integer || Value.type == json_null ||
				(Value.type == json_string && str_length(Value.u.string.ptr) == (int)Value.u.string.length))
				Id = Encode(Value);
		}
		return RpcError(Id, -32000, "MCP request timed out. An operation may have already executed; inspect editor state before retrying.");
	}
	std::string TextResult(const std::string &Json)
	{
		const bool Object = !Json.empty() && Json.front() == '{';
		return "{\"content\":[{\"type\":\"text\",\"text\":" + Quote(Json) + "}],\"structuredContent\":" + (Object ? Json : "{\"result\":" + Json + "}") + "}";
	}
	bool Dispatch(CEditorMcpServer::CRequest &Request, const FToolHandler &ToolHandler)
	{
		auto Message = Parse(Request.m_Body);
		if(!Message)
		{
			Request.m_Response = RpcError("null", -32700, "Invalid JSON");
			return true;
		}
		const auto &IdValue = (*Message)["id"];
		const std::string Id = Encode(IdValue);
		const std::string Method = String(&(*Message)["method"]);
		if(Message->type != json_object || str_comp(String(&(*Message)["jsonrpc"]), "2.0") || Method.empty() || (IdValue.type == json_string && str_length(IdValue.u.string.ptr) != (int)IdValue.u.string.length) || (IdValue.type != json_none && IdValue.type != json_string && IdValue.type != json_integer && IdValue.type != json_null))
		{
			Request.m_Response = RpcError("null", -32600, "Invalid JSON-RPC request");
			return true;
		}
		if(IdValue.type == json_none)
		{
			Request.m_Status = 202;
			return true;
		}
		auto Empty = Parse("{}");
		const auto &Params = (*Message)["params"].type == json_none ? *Empty : (*Message)["params"];
		if(Params.type != json_object)
		{
			Request.m_Response = RpcError(Id, -32602, "params must be an object");
			return true;
		}
		if(Method == "initialize")
		{
			std::string Version = String(&Params["protocolVersion"]);
			if(Version != "2025-03-26" && Version != "2025-06-18")
				Version = "2025-06-18";
			Request.m_Response = Rpc(Id, "{\"protocolVersion\":" + Quote(Version) + ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"ddnet-editor\",\"version\":\"3.0.0\"},\"instructions\":\"Inspect the user's open map before editing. editor_operations documents every tool and coordinate convention. editor_viewport shows the map; editor_constants supplies tile IDs. Connecting does not edit the map.\"}");
			return true;
		}
		if(Method == "ping")
		{
			Request.m_Response = Rpc(Id, "{}");
			return true;
		}
		if(Method == "tools/list")
		{
			Request.m_Response = Rpc(Id, ToolCatalog());
			return true;
		}
		if(Method != "tools/call")
		{
			Request.m_Response = RpcError(Id, -32601, "Unknown method");
			return true;
		}
		const std::string Name = String(&Params["name"]);
		const auto &Arguments = Params["arguments"].type == json_none ? *Empty : Params["arguments"];
		static const auto Catalog = Parse(ToolCatalog());
		const auto &Tools = (*Catalog)["tools"];
		const json_value *pTool = nullptr;
		for(unsigned i = 0; i < Tools.u.array.length; ++i)
			if(Name == String(&Tools[i]["name"]))
				pTool = &Tools[i];
		if(!pTool || !Validate(Arguments, (*pTool)["inputSchema"]))
		{
			Request.m_Response = RpcError(Id, -32602, "Unknown tool or invalid arguments; consult tools/list for its schema");
			return true;
		}
		if(Name == "editor_operations")
		{
			Request.m_Response = Rpc(Id, TextResult(OperationCatalog()));
			return true;
		}
		return ToolHandler(Name, Arguments, Id, Request);
	}
}
