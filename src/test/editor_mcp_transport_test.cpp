#include <base/net.h>
#include <base/time.h>

#include <game/editor/mcp_protocol.h>
#include <game/editor/mcp_server.h>
#include <game/editor/mcp_tools.h>

#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <thread>

namespace
{
	class CTransportTest : public testing::Test
	{
	protected:
		CEditorMcpServer m_Server;
		NETSOCKET m_Client = nullptr;
		int m_Port = 0;
		unsigned m_Calls = 0;
		void SetUp() override
		{
			for(int Port = 24000; Port < 25000; ++Port)
				if(m_Server.Start(Port))
				{
					m_Port = Port;
					break;
				}
			ASSERT_NE(m_Port, 0);
		}
		void TearDown() override
		{
			if(m_Client)
				net_tcp_close(m_Client);
		}
		void Connect()
		{
			NETADDR Bind{};
			Bind.type = NETTYPE_IPV4;
			m_Client = net_tcp_create(Bind);
			ASSERT_NE(m_Client, nullptr);
			NETADDR Address{};
			ASSERT_EQ(net_addr_from_str(&Address, ("127.0.0.1:" + std::to_string(m_Port)).c_str()), 0);
			ASSERT_EQ(net_tcp_connect(m_Client, &Address), 0);
			ASSERT_EQ(net_set_non_blocking(m_Client), 0);
		}
		std::string Headers(const std::string &Extra = "", const std::string &Method = "POST") const
		{
			return Method + " /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(m_Port) + "\r\nAccept: application/json, text/event-stream\r\nContent-Type: application/json\r\nContent-Length: 2\r\n" + Extra + "\r\n";
		}
		bool Handler(CEditorMcpServer::CRequest &Request)
		{
			++m_Calls;
			Request.m_Response = "{\"result\":true}";
			return true;
		}
		std::string Receive(const CEditorMcpServer::FHandler &Handler, int64_t Now = 0)
		{
			std::string Result;
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while(std::chrono::steady_clock::now() < Deadline)
			{
				m_Server.Update(Handler, Now);
				char aBuffer[4096];
				const int Size = net_tcp_recv(m_Client, aBuffer, sizeof(aBuffer));
				if(Size == 0)
					return Result;
				if(Size > 0)
					Result.append(aBuffer, Size);
				else
					EXPECT_TRUE(net_would_block());
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			ADD_FAILURE() << "Transport response timed out";
			return Result;
		}
		std::string Exchange(const std::string &Request)
		{
			Connect();
			EXPECT_EQ(net_tcp_send(m_Client, Request.data(), Request.size()), (int)Request.size());
			return Receive([&](CEditorMcpServer::CRequest &Message) { return Handler(Message); });
		}
	};
}

TEST_F(CTransportTest, InertStartAndPortConflict)
{
	EXPECT_EQ(m_Server.RequestCount(), 0u);
	EXPECT_EQ(m_Calls, 0u);
	CEditorMcpServer Other;
	EXPECT_FALSE(Other.Start(m_Port));
	EXPECT_FALSE(Other.Error().empty());
	EXPECT_FALSE(Other.Start(0));
	EXPECT_FALSE(Other.Start(65536));
}

TEST_F(CTransportTest, FragmentedRequestDispatchesOnce)
{
	Connect();
	const std::string First = Headers() + "{";
	ASSERT_EQ(net_tcp_send(m_Client, First.data(), First.size()), (int)First.size());
	m_Server.Update([&](CEditorMcpServer::CRequest &Message) { return Handler(Message); });
	EXPECT_EQ(m_Calls, 0u);
	ASSERT_EQ(net_tcp_send(m_Client, "}", 1), 1);
	const std::string Response = Receive([&](CEditorMcpServer::CRequest &Message) {
		EXPECT_EQ(Message.m_Body, "{}");
		return Handler(Message);
	});
	EXPECT_EQ(Response.find("HTTP/1.1 200"), 0u);
	EXPECT_NE(Response.find("{\"result\":true}"), std::string::npos);
	EXPECT_EQ(m_Calls, 1u);
	EXPECT_EQ(m_Server.RequestCount(), 1u);
}

TEST_F(CTransportTest, RejectsRemoteOrigin)
{
	const std::string Response = Exchange(Headers("Origin: https://example.com\r\n") + "{}");
	EXPECT_EQ(Response.find("HTTP/1.1 403"), 0u);
	EXPECT_EQ(m_Calls, 0u);
}

TEST_F(CTransportTest, RejectsReboundHost)
{
	std::string Request = Headers() + "{}";
	Request.replace(Request.find("127.0.0.1:"), std::string("127.0.0.1:").size(), "example.com:");
	EXPECT_EQ(Exchange(Request).find("HTTP/1.1 403"), 0u);
	EXPECT_EQ(m_Calls, 0u);
}

TEST_F(CTransportTest, RejectsDuplicateLength)
{
	EXPECT_EQ(Exchange(Headers("Content-Length: 2\r\n") + "{}").find("HTTP/1.1 400"), 0u);
	EXPECT_EQ(m_Calls, 0u);
}

TEST_F(CTransportTest, GetReturns405)
{
	EXPECT_EQ(Exchange(Headers("", "GET")).find("HTTP/1.1 405"), 0u);
	EXPECT_EQ(m_Calls, 0u);
}

TEST_F(CTransportTest, RejectsOversizedBodyBeforeRead)
{
	std::string Request = Headers();
	Request.replace(Request.find("Content-Length: 2"), std::string("Content-Length: 2").size(), "Content-Length: 99999999999999999999");
	EXPECT_EQ(Exchange(Request).find("HTTP/1.1 413"), 0u);
	EXPECT_EQ(m_Calls, 0u);
}

TEST_F(CTransportTest, NotificationResponseHasNoBody)
{
	Connect();
	const std::string Request = Headers() + "{}";
	ASSERT_EQ(net_tcp_send(m_Client, Request.data(), Request.size()), (int)Request.size());
	const std::string Response = Receive([](CEditorMcpServer::CRequest &Message) {
		Message.m_Status = 202;
		return true;
	});
	EXPECT_EQ(Response.find("HTTP/1.1 202"), 0u);
	EXPECT_NE(Response.find("Content-Length: 0\r\n"), std::string::npos);
}

TEST_F(CTransportTest, DeferredResponseAndStopDuringHandler)
{
	Connect();
	const std::string Request = Headers() + "{}";
	ASSERT_EQ(net_tcp_send(m_Client, Request.data(), Request.size()), (int)Request.size());
	const std::string Response = Receive([&](CEditorMcpServer::CRequest &Message) {
		if(++m_Calls == 1)
			return false;
		Message.m_Response = "{}";
		m_Server.Stop(); // Native editor actions can close the editor in a handler.
		return true;
	});
	EXPECT_EQ(m_Calls, 2u);
	EXPECT_EQ(Response.find("HTTP/1.1 200"), 0u);
	EXPECT_FALSE(m_Server.Running());
}

TEST_F(CTransportTest, DeferredTimeoutReturnsJsonWithoutResumingHandler)
{
	Connect();
	const std::string Body = R"({"jsonrpc":"2.0","id":"capture-\"1\"","method":"tools/call"})";
	std::string Request = Headers();
	Request.replace(Request.find("Content-Length: 2"), std::string("Content-Length: 2").size(), "Content-Length: " + std::to_string(Body.size()));
	Request += Body;
	ASSERT_EQ(net_tcp_send(m_Client, Request.data(), Request.size()), (int)Request.size());
	const auto Handler = [&](CEditorMcpServer::CRequest &) { ++m_Calls; return false; };
	const int64_t Now = time_get();
	m_Server.Update(Handler, Now);
	ASSERT_EQ(m_Calls, 1u);
	const std::string Response = Receive(Handler, Now + 31 * time_freq());
	EXPECT_EQ(Response.find("HTTP/1.1 408 Request Timeout"), 0u);
	const size_t BodyStart = Response.find("\r\n\r\n");
	ASSERT_NE(BodyStart, std::string::npos);
	const std::string ResponseBody = Response.substr(BodyStart + 4);
	auto *pResponse = JsonParse(ResponseBody.c_str(), ResponseBody.size());
	ASSERT_NE(pResponse, nullptr);
	EXPECT_STREQ((*pResponse)["id"].u.string.ptr, "capture-\"1\"");
	EXPECT_EQ((*pResponse)["error"]["code"].u.integer, -32000);
	EXPECT_NE(std::string((*pResponse)["error"]["message"].u.string.ptr).find("inspect editor state"), std::string::npos);
	json_value_free(pResponse);
	EXPECT_EQ(m_Calls, 1u);
	EXPECT_EQ(m_Server.RequestCount(), 1u);
}

TEST_F(CTransportTest, IncompleteRequestTimeoutReturns408WithoutDispatch)
{
	Connect();
	const std::string Request = Headers() + "{";
	ASSERT_EQ(net_tcp_send(m_Client, Request.data(), Request.size()), (int)Request.size());
	const auto Handler = [&](CEditorMcpServer::CRequest &Message) { return this->Handler(Message); };
	const int64_t Now = time_get();
	m_Server.Update(Handler, Now);
	const std::string Response = Receive(Handler, Now + 31 * time_freq());
	EXPECT_EQ(Response.find("HTTP/1.1 408"), 0u);
	EXPECT_NE(Response.find("\"id\":null"), std::string::npos);
	EXPECT_EQ(m_Calls, 0u);
}

TEST_F(CTransportTest, QueuedTimeoutDoesNotDispatchAndServerRecovers)
{
	Connect();
	const std::string Request = Headers() + "{}";
	ASSERT_EQ(net_tcp_send(m_Client, Request.data(), Request.size()), (int)Request.size());
	const auto Pending = [&](CEditorMcpServer::CRequest &) { ++m_Calls; return false; };
	const int64_t Now = time_get();
	m_Server.Update(Pending, Now);
	ASSERT_EQ(m_Calls, 1u);
	NETSOCKET First = m_Client;
	m_Client = nullptr;
	Connect();
	ASSERT_EQ(net_tcp_send(m_Client, Request.data(), Request.size()), (int)Request.size());
	m_Server.Update(Pending, Now);
	ASSERT_EQ(m_Calls, 2u); // Only the first connection was resumed.
	EXPECT_EQ(Receive(Pending, Now + 31 * time_freq()).find("HTTP/1.1 408"), 0u);
	EXPECT_EQ(m_Calls, 2u);
	EXPECT_EQ(m_Server.RequestCount(), 1u);
	net_tcp_close(First);
	net_tcp_close(m_Client);
	m_Client = nullptr;
	EXPECT_EQ(Exchange(Request).find("HTTP/1.1 200"), 0u);
	EXPECT_EQ(m_Server.RequestCount(), 2u);
}

TEST(EditorMcpProtocol, TimeoutResponsePreservesOnlyValidRequestIds)
{
	for(const char *pBody : {
		    R"({"id":9007199254740991})",
		    R"({"id":"capture"})",
		    R"({"id":null})",
		    R"({"id":true})",
		    R"({"id":[]})",
		    R"({"id":"invalid\u0000id"})",
		    "{", "[]", "{}"})
	{
		const std::string Response = EditorMcp::TimeoutResponse(pBody);
		auto *pResponse = JsonParse(Response.c_str(), Response.size());
		ASSERT_NE(pResponse, nullptr);
		EXPECT_EQ((*pResponse)["error"]["code"].u.integer, -32000);
		if(std::string(pBody) == R"({"id":9007199254740991})")
			EXPECT_EQ((*pResponse)["id"].u.integer, 9007199254740991);
		else if(std::string(pBody) == R"({"id":"capture"})")
			EXPECT_STREQ((*pResponse)["id"].u.string.ptr, "capture");
		else
			EXPECT_EQ((*pResponse)["id"].type, json_null);
		json_value_free(pResponse);
	}
}

TEST(EditorMcpProtocol, InitializeAndDiscoveryNeverInvokeEditor)
{
	unsigned Calls = 0;
	auto Handler = [&](const std::string &, const json_value &, const std::string &, CEditorMcpServer::CRequest &) { ++Calls; return true; };
	for(const char *pBody : {
		    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})",
		    R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})",
		    R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"editor_operations"}})"})
	{
		CEditorMcpServer::CRequest Request;
		Request.m_Body = pBody;
		EXPECT_TRUE(EditorMcp::Dispatch(Request, Handler));
		auto *pResponse = JsonParse(Request.m_Response.c_str(), Request.m_Response.size());
		ASSERT_NE(pResponse, nullptr);
		EXPECT_EQ((*pResponse)["error"].type, json_none);
		if((*pResponse)["id"].u.integer == 2)
		{
			EXPECT_EQ((*pResponse)["result"]["tools"].type, json_array);
			EXPECT_EQ((*pResponse)["result"]["tools"].u.array.length, 90u);
		}
		json_value_free(pResponse);
	}
	EXPECT_EQ(Calls, 0u);
}

TEST(EditorMcpProtocol, InvalidInputNeverInvokesEditor)
{
	unsigned Calls = 0;
	auto Handler = [&](const std::string &, const json_value &, const std::string &, CEditorMcpServer::CRequest &) { ++Calls; return true; };
	for(const char *pBody : {
		    "{", "{}",
		    R"({"jsonrpc":"2.0","id":1,"method":"tools/call"})",
		    R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"editor_tiles_set","arguments":{"group":0,"layer":0,"index":256}}})",
		    R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"editor_tiles_set","arguments":{"group":0,"layer":0,"index":true}}})",
		    R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"editor_commands","arguments":{"operations":[{"op":"group_add"},{"op":"not_an_operation"}]}}})"})
	{
		CEditorMcpServer::CRequest Request;
		Request.m_Body = pBody;
		EXPECT_TRUE(EditorMcp::Dispatch(Request, Handler));
		auto *pResponse = JsonParse(Request.m_Response.c_str(), Request.m_Response.size());
		ASSERT_NE(pResponse, nullptr);
		EXPECT_EQ((*pResponse)["error"].type, json_object);
		json_value_free(pResponse);
	}
	EXPECT_EQ(Calls, 0u);
}

TEST(EditorMcpProtocol, NamedToolDispatchAndLargeRequestId)
{
	CEditorMcpServer::CRequest Request;
	Request.m_Body = R"({"jsonrpc":"2.0","id":9007199254740991,"method":"tools/call","params":{"name":"editor_tiles_set","arguments":{"group":0,"layer":1,"x":2,"y":3,"index":4}}})";
	bool Called = false;
	EXPECT_TRUE(EditorMcp::Dispatch(Request, [&](const std::string &Name, const json_value &Arguments, const std::string &Id, CEditorMcpServer::CRequest &Message) {
		Called = true;
		EXPECT_EQ(Name, "editor_tiles_set");
		EXPECT_EQ(Arguments["index"].u.integer, 4);
		EXPECT_EQ(Id, "9007199254740991");
		Message.m_Response = EditorMcp::Rpc(Id, EditorMcp::TextResult("null"));
		return true;
	}));
	EXPECT_TRUE(Called);
	EXPECT_NE(Request.m_Response.find("9007199254740991"), std::string::npos);
}

TEST(EditorMcpProtocol, NotificationAndUnknownMethod)
{
	auto Handler = [](const std::string &, const json_value &, const std::string &, CEditorMcpServer::CRequest &) { ADD_FAILURE(); return true; };
	CEditorMcpServer::CRequest Notification;
	Notification.m_Body = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
	EXPECT_TRUE(EditorMcp::Dispatch(Notification, Handler));
	EXPECT_EQ(Notification.m_Status, 202);
	EXPECT_TRUE(Notification.m_Response.empty());
	CEditorMcpServer::CRequest Request;
	Request.m_Body = R"({"jsonrpc":"2.0","id":"abc","method":"unknown"})";
	EXPECT_TRUE(EditorMcp::Dispatch(Request, Handler));
	EXPECT_NE(Request.m_Response.find("-32601"), std::string::npos);
}

TEST(EditorMcpProtocol, CatalogIsConsistent)
{
	const std::string &ToolText = EditorMcp::ToolCatalog();
	const std::string &OperationText = EditorMcp::OperationCatalog();
	json_value *pTools = JsonParse(ToolText.c_str(), ToolText.size());
	json_value *pOperations = JsonParse(OperationText.c_str(), OperationText.size());
	ASSERT_NE(pTools, nullptr);
	ASSERT_NE(pOperations, nullptr);

	const json_value &Tools = (*pTools)["tools"];
	ASSERT_EQ(Tools.type, json_array);
	std::set<std::string> Names;
	const json_value *pCommands = nullptr;
	for(unsigned i = 0; i < Tools.u.array.length; ++i)
	{
		const json_value &Tool = Tools[i];
		ASSERT_EQ(Tool["name"].type, json_string);
		const std::string Name = Tool["name"].u.string.ptr;
		EXPECT_TRUE(Names.insert(Name).second) << "duplicate tool " << Name;
		EXPECT_EQ(Name.rfind("editor_", 0), 0u) << Name;
		ASSERT_EQ(Tool["description"].type, json_string) << Name;
		EXPECT_GT(Tool["description"].u.string.length, 0u) << Name;
		EXPECT_EQ(Tool["inputSchema"]["type"].type, json_string) << Name;
		EXPECT_EQ(Tool["inputSchema"]["properties"].type, json_object) << Name;
		// Every required field must be a declared property.
		const json_value &Required = Tool["inputSchema"]["required"];
		ASSERT_EQ(Required.type, json_array) << Name;
		for(unsigned j = 0; j < Required.u.array.length; ++j)
			EXPECT_NE(Tool["inputSchema"]["properties"][static_cast<const char *>(Required[j].u.string.ptr)].type, json_none) << Name;
		if(Name == "editor_commands")
			pCommands = &Tool["inputSchema"]["properties"]["operations"]["items"]["oneOf"];
	}

	// Each operation is a tool, a batch command and an editor_operations entry.
	const json_value &Operations = (*pOperations)["operations"];
	ASSERT_EQ(Operations.type, json_object);
	ASSERT_NE(pCommands, nullptr);
	ASSERT_EQ(pCommands->type, json_array);
	EXPECT_EQ(pCommands->u.array.length, Operations.u.object.length);
	EXPECT_EQ(Tools.u.array.length, Operations.u.object.length + 8);
	for(unsigned i = 0; i < Operations.u.object.length; ++i)
	{
		const std::string Name = Operations.u.object.values[i].name;
		EXPECT_EQ(Names.count("editor_" + Name), 1u) << Name;
		EXPECT_EQ(Name, (*pCommands)[i]["properties"]["op"]["const"].u.string.ptr);
	}
	EXPECT_EQ((*pOperations)["conventions"].type, json_object);

	json_value_free(pTools);
	json_value_free(pOperations);
}
