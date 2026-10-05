#include "mcp_server.h"

#include "mcp_protocol.h"

#include <base/net.h>
#include <base/str.h>
#include <base/time.h>

#include <algorithm>
#include <cctype>
#include <map>

namespace
{
	constexpr size_t MAX_BODY = 32 * 1024 * 1024;
	constexpr size_t MAX_HEADERS = 16 * 1024;
	constexpr size_t FRAME_BUDGET = 256 * 1024;
	std::string Lower(std::string Value)
	{
		for(char &C : Value)
			C = std::tolower((unsigned char)C);
		return Value;
	}
}

CEditorMcpServer::~CEditorMcpServer()
{
	Stop();
}

bool CEditorMcpServer::Start(int Port)
{
	Stop();
	m_Error.clear();
	if(Port < 1 || Port > 65535)
	{
		m_Error = "Choose a port between 1 and 65535.";
		return false;
	}
	NETADDR Address{};
	Address.type = NETTYPE_IPV4;
	Address.ip[0] = 127;
	Address.ip[3] = 1;
	Address.port = Port;
	m_Socket = net_tcp_create(Address);
	if(!m_Socket || net_tcp_listen(m_Socket, 16) != 0 || net_set_non_blocking(m_Socket) != 0)
	{
		Stop();
		m_Error = "Could not listen on this port. Choose another port.";
		return false;
	}
	m_Port = Port;
	m_RequestCount = 0;
	return true;
}

void CEditorMcpServer::Stop()
{
	if(m_Updating)
	{
		m_StopRequested = true;
		return;
	}
	m_StopRequested = false;
	for(auto &Connection : m_vConnections)
		net_tcp_close(Connection.m_Socket);
	m_vConnections.clear();
	if(m_Socket)
		net_tcp_close(m_Socket);
	m_Socket = nullptr;
}

void CEditorMcpServer::Respond(CConnection &Connection, int Status, const std::string &Body)
{
	const char *pReason = Status == 200 ? "OK" : Status == 202 ? "Accepted" :
					     Status == 400         ? "Bad Request" :
					     Status == 403         ? "Forbidden" :
					     Status == 404         ? "Not Found" :
					     Status == 405         ? "Method Not Allowed" :
					     Status == 406         ? "Not Acceptable" :
					     Status == 413         ? "Content Too Large" :
					     Status == 415         ? "Unsupported Media Type" :
								     "Request Timeout";
	Connection.m_Output = "HTTP/1.1 " + std::to_string(Status) + " " + pReason + "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(Body.size()) + "\r\n";
	if(Status == 405)
		Connection.m_Output += "Allow: POST\r\n";
	Connection.m_Output += "\r\n" + Body;
}

void CEditorMcpServer::ParseHeaders(CConnection &Connection)
{
	const size_t End = Connection.m_Input.find("\r\n\r\n");
	if(End == std::string::npos)
	{
		if(Connection.m_Input.size() > MAX_HEADERS)
			Respond(Connection, 413);
		return;
	}
	if(End > MAX_HEADERS)
	{
		Respond(Connection, 413);
		return;
	}
	Connection.m_HeaderSize = End + 4;
	const size_t FirstEnd = Connection.m_Input.find("\r\n");
	const std::string First = Connection.m_Input.substr(0, FirstEnd);
	const size_t Space = First.find(' '), LastSpace = First.rfind(' ');
	if(Space == std::string::npos || Space == LastSpace || First.substr(LastSpace + 1) != "HTTP/1.1")
	{
		Respond(Connection, 400);
		return;
	}
	std::map<std::string, std::string> Headers;
	for(size_t Start = FirstEnd + 2; Start < End;)
	{
		const size_t LineEnd = Connection.m_Input.find("\r\n", Start);
		const std::string Line = Connection.m_Input.substr(Start, LineEnd - Start);
		const size_t Colon = Line.find(':');
		if(Colon == std::string::npos)
		{
			Respond(Connection, 400);
			return;
		}
		std::string Key = Lower(Line.substr(0, Colon)), Value = Line.substr(Colon + 1);
		while(!Value.empty() && (Value.front() == ' ' || Value.front() == '\t'))
			Value.erase(Value.begin());
		while(!Value.empty() && (Value.back() == ' ' || Value.back() == '\t'))
			Value.pop_back();
		if(Headers.count(Key))
		{
			Respond(Connection, 400);
			return;
		}
		Headers[Key] = Value;
		Start = LineEnd + 2;
	}
	const std::string Authority = "127.0.0.1:" + std::to_string(m_Port), Localhost = "localhost:" + std::to_string(m_Port);
	if(Headers["host"] != Authority && Headers["host"] != Localhost)
	{
		Respond(Connection, 403);
		return;
	}
	if(Headers.count("origin") && Headers["origin"] != "http://" + Authority && Headers["origin"] != "http://" + Localhost)
	{
		Respond(Connection, 403);
		return;
	}
	if(First.substr(Space + 1, LastSpace - Space - 1) != "/mcp")
	{
		Respond(Connection, 404);
		return;
	}
	if(First.substr(0, Space) != "POST")
	{
		Respond(Connection, 405);
		return;
	}
	if(Headers.count("mcp-protocol-version") && Headers["mcp-protocol-version"] != "2025-03-26" && Headers["mcp-protocol-version"] != "2025-06-18")
	{
		Respond(Connection, 400);
		return;
	}
	if(Headers["accept"].find("application/json") == std::string::npos)
	{
		Respond(Connection, 406);
		return;
	}
	const std::string ContentType = Lower(Headers["content-type"]);
	if(ContentType != "application/json" && ContentType.rfind("application/json;", 0) != 0)
	{
		Respond(Connection, 415);
		return;
	}
	if(Headers.count("transfer-encoding") || Headers["content-length"].empty())
	{
		Respond(Connection, 400);
		return;
	}
	for(char C : Headers["content-length"])
	{
		if(C < '0' || C > '9')
		{
			Respond(Connection, 400);
			return;
		}
		Connection.m_ContentLength = Connection.m_ContentLength * 10 + C - '0';
		if(Connection.m_ContentLength > MAX_BODY)
		{
			Respond(Connection, 413);
			return;
		}
	}
}

void CEditorMcpServer::Update(const FHandler &Handler, int64_t Now)
{
	if(!Running())
		return;
	if(Now == 0)
		Now = time_get();
	m_Updating = true;
	while(m_vConnections.size() < 16)
	{
		NETSOCKET Socket;
		NETADDR Address;
		if(net_tcp_accept(m_Socket, &Socket, &Address) < 0)
			break;
		net_set_non_blocking(Socket);
		CConnection Connection;
		Connection.m_Socket = Socket;
		Connection.m_Deadline = Now + 30 * time_freq();
		m_vConnections.push_back(std::move(Connection));
	}
	// Serialize editor operations, including asynchronous captures, across clients.
	bool Dispatched = false;
	for(auto &Connection : m_vConnections)
	{
		if(Connection.m_Dispatched && Connection.m_Output.empty())
		{
			Dispatched = true;
			if(Now <= Connection.m_Deadline && Handler(Connection.m_Request))
				Respond(Connection, Connection.m_Request.m_Status, Connection.m_Request.m_Response);
			break;
		}
	}
	for(auto It = m_vConnections.begin(); It != m_vConnections.end();)
	{
		auto &Connection = *It;
		bool Close = false;
		if(Now > Connection.m_Deadline)
		{
			if(Connection.m_Sent != 0)
				Close = true; // A partially sent HTTP response cannot be replaced.
			else
			{
				Respond(Connection, 408, EditorMcp::TimeoutResponse(Connection.m_Request.m_Body));
				Connection.m_Deadline = Now + 5 * time_freq();
			}
		}
		if(!Close && !Connection.m_Ready && Connection.m_Output.empty())
		{
			char aBuffer[16384];
			for(size_t Read = 0; Read < FRAME_BUDGET;)
			{
				const int Size = net_tcp_recv(Connection.m_Socket, aBuffer, sizeof(aBuffer));
				if(Size <= 0)
				{
					Close = Size == 0 || !net_would_block();
					break;
				}
				Read += Size;
				Connection.m_Input.append(aBuffer, Size);
				if(!Connection.m_HeaderSize)
					ParseHeaders(Connection);
				if(!Connection.m_Output.empty())
					break;
				if(Connection.m_HeaderSize && Connection.m_Input.size() >= Connection.m_HeaderSize + Connection.m_ContentLength)
				{
					Connection.m_Request.m_Body = Connection.m_Input.substr(Connection.m_HeaderSize, Connection.m_ContentLength);
					Connection.m_Input.clear();
					Connection.m_Ready = true;
					break;
				}
			}
		}
		if(!Close && Connection.m_Ready && !Dispatched && Connection.m_Output.empty())
		{
			Dispatched = true;
			Connection.m_Dispatched = true;
			++m_RequestCount;
			if(Handler(Connection.m_Request))
				Respond(Connection, Connection.m_Request.m_Status, Connection.m_Request.m_Response);
		}
		if(!Close && !Connection.m_Output.empty())
		{
			const size_t Remaining = Connection.m_Output.size() - Connection.m_Sent;
			const int Sent = net_tcp_send(Connection.m_Socket, Connection.m_Output.data() + Connection.m_Sent, std::min(Remaining, FRAME_BUDGET));
			if(Sent > 0)
				Connection.m_Sent += Sent;
			else if(Sent == 0 || !net_would_block())
				Close = true;
			Close |= Connection.m_Sent == Connection.m_Output.size();
		}
		if(Close)
		{
			net_tcp_close(Connection.m_Socket);
			It = m_vConnections.erase(It);
		}
		else
			++It;
	}
	m_Updating = false;
	if(m_StopRequested)
		Stop();
}
