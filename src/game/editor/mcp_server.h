#ifndef GAME_EDITOR_MCP_SERVER_H
#define GAME_EDITOR_MCP_SERVER_H

#include <base/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Nonblocking HTTP transport. All handlers run on the editor thread.
class CEditorMcpServer
{
public:
	struct CRequest
	{
		std::string m_Body;
		std::string m_Response;
		std::shared_ptr<void> m_pUserState;
		int m_Status = 200;
	};
	using FHandler = std::function<bool(CRequest &)>;
	CEditorMcpServer() = default;
	~CEditorMcpServer();
	bool Start(int Port);
	void Stop();
	// Now defaults to the current clock; an explicit timestamp allows deterministic deadline tests.
	void Update(const FHandler &Handler, int64_t Now = 0);
	bool Running() const { return m_Socket != nullptr; }
	int Port() const { return m_Port; }
	const std::string &Error() const { return m_Error; }
	unsigned RequestCount() const { return m_RequestCount; }

private:
	struct CConnection
	{
		NETSOCKET m_Socket = nullptr;
		std::string m_Input;
		std::string m_Output;
		size_t m_Sent = 0;
		size_t m_HeaderSize = 0;
		size_t m_ContentLength = 0;
		int64_t m_Deadline = 0;
		bool m_Ready = false;
		bool m_Dispatched = false;
		CRequest m_Request;
	};
	bool m_Updating = false;
	bool m_StopRequested = false;
	NETSOCKET m_Socket = nullptr;
	int m_Port = 0;
	std::string m_Error;
	unsigned m_RequestCount = 0;
	std::vector<CConnection> m_vConnections;
	void ParseHeaders(CConnection &Connection);
	static void Respond(CConnection &Connection, int Status, const std::string &Body = "");
};

#endif
