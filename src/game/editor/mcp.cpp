#include "mcp.h"

#include "automation.h"
#include "editor.h"
#include "mapitems/image.h"
#include "mapitems/sound.h"
#include "mcp_protocol.h"
#include "mcp_server.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/gfx/image_loader.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/storage.h>

#include <algorithm>
#include <climits>
#include <cmath>

namespace
{
	using namespace EditorMcp;
	using PJson = std::unique_ptr<json_value, decltype(&json_value_free)>;
	PJson Parse(const std::string &Text)
	{
		return PJson(JsonParse(Text.c_str(), Text.size()), json_value_free);
	}
	const char *String(const json_value *pValue)
	{
		return pValue->type == json_string ? pValue->u.string.ptr : "";
	}
	std::string ToolError(const std::string &Message, const std::vector<std::string> &Results = {})
	{
		std::string Completed = "[";
		for(size_t i = 0; i < Results.size(); ++i)
			Completed += (i ? "," : "") + Results[i];
		return "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":" + Quote("{\"error\":" + Quote(Message) + ",\"completed_results\":" + Completed + "]}") + "}]}";
	}
	std::string Base64(const void *pData, size_t Size)
	{
		std::string Result((Size + 2) / 3 * 4 + 1, '\0');
		str_base64(Result.data(), Result.size(), pData, Size);
		Result.pop_back();
		return Result;
	}
	std::string MediaResult(const std::string &Data, const char *pType, const char *pMime, const std::string &Metadata)
	{
		return "{\"content\":[{\"type\":\"text\",\"text\":" + Quote(Metadata) + "},{\"type\":" + Quote(pType) + ",\"mimeType\":" + Quote(pMime) + ",\"data\":" + Quote(Data) + "}],\"structuredContent\":" + Metadata + "}";
	}
	std::string ImageResult(CImageInfo &Image, int MaxSize, const std::string &Extra = "")
	{
		const size_t OriginalWidth = Image.m_Width, OriginalHeight = Image.m_Height;
		if(std::max(Image.m_Width, Image.m_Height) > (size_t)MaxSize)
		{
			const double Scale = MaxSize / (double)std::max(Image.m_Width, Image.m_Height);
			ResizeImage(Image, std::max(1, (int)std::round(Image.m_Width * Scale)), std::max(1, (int)std::round(Image.m_Height * Scale)));
		}
		CByteBufferWriter Writer;
		if(!CImageLoader::SavePng(Writer, Image))
			return ToolError("PNG encoding failed");
		const std::string Metadata = "{\"original_size\":[" + std::to_string(OriginalWidth) + "," + std::to_string(OriginalHeight) + "],\"image_size\":[" + std::to_string(Image.m_Width) + "," + std::to_string(Image.m_Height) + "]" + Extra + "}";
		return MediaResult(Base64(Writer.Data(), Writer.Size()), "image", "image/png", Metadata);
	}
	std::string TemporaryName(const char *pExtension)
	{
		static unsigned Sequence = 0;
		return "mcp-" + std::to_string(time_get()) + "-" + std::to_string(++Sequence) + pExtension;
	}
	bool Run(CEditor *pEditor, const std::string &Commands, std::vector<std::string> &Results, std::string &Error)
	{
		auto Json = Parse(Commands);
		if(!Json)
		{
			Error = "Invalid operation JSON";
			return false;
		}
		return RunEditorOperations(pEditor, *Json, Results, Error);
	}
	struct CJob
	{
		std::string m_Id;
		std::string m_Path;
		std::string m_Resolved;
		std::string m_Result;
		bool m_Save = false;
		bool m_IncludeUi = false;
		int m_MaxSize = 1200;
		int m_X = 0, m_Y = 0, m_Width = 0, m_Height = 0;
		int64_t m_Deadline = time_get() + 10 * time_freq();
		~CJob()
		{
			if(!m_Save && !m_Resolved.empty())
				(void)fs_remove(m_Resolved.c_str());
		}
	};
	bool FinishJob(CEditor *pEditor, CEditorMcpServer::CRequest &Request)
	{
		auto pJob = std::static_pointer_cast<CJob>(Request.m_pUserState);
		if(pJob->m_Save)
		{
			if(pEditor->IsSaving(pJob->m_Path.c_str()))
				return false;
			const auto It = pEditor->m_AutomationSaveResults.find(pJob->m_Path);
			if(It == pEditor->m_AutomationSaveResults.end())
				Request.m_Response = Rpc(pJob->m_Id, ToolError("Save completion status expired"));
			else if(!It->second.empty())
				Request.m_Response = Rpc(pJob->m_Id, ToolError(It->second));
			else
			{
				IOHANDLE File = io_open(pJob->m_Resolved.c_str(), IOFLAG_READ);
				if(!File)
					Request.m_Response = Rpc(pJob->m_Id, ToolError("Saved file is missing"));
				else
				{
					const long Size = io_length(File);
					io_close(File);
					Request.m_Response = Rpc(pJob->m_Id, TextResult("{\"path\":" + Quote(pJob->m_Resolved) + ",\"bytes\":" + std::to_string(Size) + "}"));
				}
			}
			return true;
		}
		IOHANDLE File = io_open(pJob->m_Resolved.c_str(), IOFLAG_READ);
		if(!File)
		{
			if(time_get() > pJob->m_Deadline)
			{
				Request.m_Response = Rpc(pJob->m_Id, ToolError("Screenshot capture did not finish within 10 seconds; check the editor log"));
				return true;
			}
			return false;
		}
		// The screenshot worker closes its file only after writing the IEND chunk.
		const long Size = io_length(File);
		unsigned char aEnd[12]{};
		if(Size >= 12)
		{
			io_seek(File, Size - 12, EIoSeekOrigin::START);
			io_read(File, aEnd, sizeof(aEnd));
		}
		if(Size < 12 || mem_comp(aEnd, "\0\0\0\0IEND\xae\x42\x60\x82", 12))
		{
			io_close(File);
			if(time_get() > pJob->m_Deadline)
			{
				Request.m_Response = Rpc(pJob->m_Id, ToolError("Screenshot file was not completed within 10 seconds; check the editor log"));
				return true;
			}
			return false;
		}
		io_seek(File, 0, EIoSeekOrigin::START);
		CImageInfo Image;
		int Incompatible;
		if(!CImageLoader::LoadPng(File, pJob->m_Resolved.c_str(), Image, Incompatible))
		{
			Request.m_Response = Rpc(pJob->m_Id, ToolError("Could not read screenshot"));
			return true;
		}
		ConvertToRgba(Image);
		if(!pJob->m_IncludeUi)
		{
			if(pJob->m_X < 0 || pJob->m_Y < 0 || pJob->m_Width <= 0 || pJob->m_Height <= 0 || (size_t)(pJob->m_X + pJob->m_Width) > Image.m_Width || (size_t)(pJob->m_Y + pJob->m_Height) > Image.m_Height)
			{
				Request.m_Response = Rpc(pJob->m_Id, ToolError("Viewport changed during capture; request another preview"));
				return true;
			}
			CImageInfo Crop;
			Crop.m_Width = pJob->m_Width;
			Crop.m_Height = pJob->m_Height;
			Crop.m_Format = CImageInfo::FORMAT_RGBA;
			Crop.Allocate();
			Crop.CopyRectFrom(Image, pJob->m_X, pJob->m_Y, Crop.m_Width, Crop.m_Height, 0, 0);
			Image = std::move(Crop);
		}
		Request.m_Response = Rpc(pJob->m_Id, ImageResult(Image, pJob->m_MaxSize, ",\"camera\":" + pJob->m_Result));
		return true;
	}
	std::string CreateTileset(CEditor *pEditor, const json_value &Arguments)
	{
		const auto &Tiles = Arguments["tiles"];
		const int TileSize = Arguments["tile_size"].type == json_integer ? Arguments["tile_size"].u.integer : 32;
		CImageInfo Image;
		Image.m_Width = Image.m_Height = TileSize * 16;
		Image.m_Format = CImageInfo::FORMAT_RGBA;
		Image.AllocateFillZero();
		for(unsigned i = 0; i < Tiles.u.object.length; ++i)
		{
			const auto &Entry = Tiles.u.object.values[i];
			int Index = 0;
			for(const char *p = Entry.name; *p; ++p)
			{
				if(*p < '0' || *p > '9' || Index > 255)
					return ToolError("Tile IDs must be 1..255");
				Index = Index * 10 + *p - '0';
			}
			if(Index < 1 || Index > 255)
				return ToolError("Tile IDs must be 1..255");
			const json_value &Value = *Entry.value;
			if(Value.type != json_array)
				return ToolError("Expected an RGBA color or pixel grid");
			const bool Solid = Value.u.array.length == 4 && Value[0].type == json_integer;
			if(!Solid && Value.u.array.length != (unsigned)TileSize)
				return ToolError("Pixel grid must match tile_size");
			for(int Y = 0; Y < TileSize; ++Y)
				for(int X = 0; X < TileSize; ++X)
				{
					if(!Solid && (Value[Y].type != json_array || Value[Y].u.array.length != (unsigned)TileSize))
						return ToolError("Pixel grid must match tile_size");
					const json_value &Color = Solid ? Value : Value[Y][X];
					if(Color.type != json_array || Color.u.array.length != 4)
						return ToolError("RGBA colors require four byte values");
					const size_t Pixel = ((Index / 16 * TileSize + Y) * Image.m_Width + Index % 16 * TileSize + X) * 4;
					for(int C = 0; C < 4; ++C)
					{
						if(Color[C].type != json_integer || Color[C].u.integer < 0 || Color[C].u.integer > 255)
							return ToolError("RGBA values must be 0..255");
						Image.m_pData[Pixel + C] = Color[C].u.integer;
					}
				}
		}
		const char *pPath = String(&Arguments["path"]);
		if(!pPath[0])
			return ToolError("path is required");
		IOHANDLE Existing = pEditor->Storage()->OpenFile(pPath, IOFLAG_READ, IStorage::TYPE_SAVE_OR_ABSOLUTE);
		if(Existing)
		{
			io_close(Existing);
			return ToolError("Atlas already exists; choose another path");
		}
		char aResolved[IO_MAX_PATH_LENGTH];
		IOHANDLE Output = pEditor->Storage()->OpenFile(pPath, IOFLAG_WRITE, IStorage::TYPE_SAVE_OR_ABSOLUTE, aResolved, sizeof(aResolved));
		if(!Output)
			return ToolError("Could not open atlas path");
		if(!CImageLoader::SavePng(Output, pPath, Image))
			return ToolError("Atlas PNG writing failed");
		return TextResult("{\"path\":" + Quote(aResolved) + "}");
	}
	bool HandleTool(CEditor *pEditor, CEditorMcpServer::CRequest &Request, const std::string &Name, const json_value &Arguments, const std::string &Id)
	{
		if(Name == "editor_create_tileset")
		{
			Request.m_Response = Rpc(Id, CreateTileset(pEditor, Arguments));
			return true;
		}
		const int MaxSize = Arguments["max_size"].type == json_integer ? Arguments["max_size"].u.integer : 1200;
		if(Name == "editor_asset_image" || Name == "editor_asset_audio")
		{
			const bool Image = Name == "editor_asset_image";
			const auto RawIndex = Arguments[Image ? "image" : "sound"].u.integer;
			const int Index = RawIndex <= INT_MAX ? (int)RawIndex : -1;
			const size_t Count = Image ? pEditor->Map()->m_vpImages.size() : pEditor->Map()->m_vpSounds.size();
			if(Index < 0 || (size_t)Index >= Count)
				Request.m_Response = Rpc(Id, ToolError("Asset index is out of range"));
			else if(Image)
			{
				auto &Source = *pEditor->Map()->m_vpImages[Index];
				if(!Source.m_pData || Source.m_Width * Source.m_Height > 16 * 1024 * 1024)
					Request.m_Response = Rpc(Id, ToolError("Asset is unavailable or exceeds preview limits; export it instead"));
				else
				{
					CImageInfo Copy = Source.DeepCopy();
					Request.m_Response = Rpc(Id, ImageResult(Copy, MaxSize, ",\"image\":" + std::to_string(Index)));
				}
			}
			else
			{
				auto &Sound = *pEditor->Map()->m_vpSounds[Index];
				if(Sound.m_DataSize > 64 * 1024 * 1024)
					Request.m_Response = Rpc(Id, ToolError("Audio exceeds preview limits; export it instead"));
				else
					Request.m_Response = Rpc(Id, MediaResult(Base64(Sound.m_pData, Sound.m_DataSize), "audio", "audio/ogg", "{\"sound\":" + std::to_string(Index) + ",\"bytes\":" + std::to_string(Sound.m_DataSize) + "}"));
			}
			return true;
		}
		std::vector<std::string> Results;
		std::string Error;
		if(Name == "editor_import_asset")
		{
			const std::string Kind = String(&Arguments["kind"]);
			const auto &Encoded = Arguments["data_base64"];
			std::vector<unsigned char> Data(Encoded.u.string.length / 4 * 3 + 3);
			const int Size = str_base64_decode(Data.data(), Data.size(), Encoded.u.string.ptr);
			if(Size < 0 || Size > 16 * 1024 * 1024)
			{
				Request.m_Response = Rpc(Id, ToolError("Invalid base64 or upload exceeds 16 MiB"));
				return true;
			}
			const std::string Path = TemporaryName(Kind == "image" ? ".png" : ".opus");
			char aResolved[IO_MAX_PATH_LENGTH];
			IOHANDLE File = pEditor->Storage()->OpenFile(Path.c_str(), IOFLAG_WRITE, IStorage::TYPE_SAVE, aResolved, sizeof(aResolved));
			if(!File)
			{
				Request.m_Response = Rpc(Id, ToolError("Could not create asset upload"));
				return true;
			}
			const bool Written = io_write(File, Data.data(), Size) == (unsigned)Size;
			io_close(File);
			const bool Replace = Arguments["replace"].type == json_integer;
			const std::string Commands = "[{\"op\":" + Quote(Kind + (Replace ? "_replace" : "_add")) + ",\"path\":" + Quote(aResolved) + ",\"name\":" + Encode(Arguments["name"]) + (Replace ? "," + Quote(Kind) + ":" + Encode(Arguments["replace"]) : "") + "}]";
			const bool Success = Written && Run(pEditor, Commands, Results, Error);
			(void)fs_remove(aResolved);
			Request.m_Response = Rpc(Id, Success ? TextResult("{\"index\":" + Results[0] + "}") : ToolError(Error.empty() ? "Asset upload failed" : Error));
			return true;
		}
		if(Name == "editor_viewport" || Name == "editor_overview")
		{
			if(Name == "editor_overview" && !Run(pEditor, "[{\"op\":\"view_fit\"}]", Results, Error))
			{
				Request.m_Response = Rpc(Id, ToolError(Error));
				return true;
			}
			Results.clear();
			auto Job = std::make_shared<CJob>();
			Job->m_Id = Id;
			Job->m_Path = TemporaryName(".png");
			Job->m_MaxSize = MaxSize;
			Job->m_IncludeUi = Arguments["include_ui"].type == json_boolean && Arguments["include_ui"].u.boolean;
			if(!Run(pEditor, "[{\"op\":\"screenshot\",\"path\":" + Quote(Job->m_Path) + "}]", Results, Error))
			{
				Request.m_Response = Rpc(Id, ToolError(Error));
				return true;
			}
			auto Capture = Parse(Results[0]);
			Job->m_Resolved = String(&(*Capture)["path"]);
			Job->m_X = json_int_get(&(*Capture)["viewport_x"]);
			Job->m_Y = json_int_get(&(*Capture)["viewport_y"]);
			Job->m_Width = json_int_get(&(*Capture)["viewport_width"]);
			Job->m_Height = json_int_get(&(*Capture)["viewport_height"]);
			Job->m_Result = "{\"x\":" + Encode((*Capture)["camera_x"]) + ",\"y\":" + Encode((*Capture)["camera_y"]) + ",\"zoom\":" + Encode((*Capture)["zoom"]) + "}";
			Request.m_pUserState = Job;
			return false;
		}
		std::string Commands;
		if(Name == "editor_commands")
			Commands = Encode(Arguments["operations"]);
		else
		{
			std::string Fields = Encode(Arguments);
			Commands = "[{\"op\":" + Quote(Name.substr(7)) + (Fields.size() > 2 ? "," + Fields.substr(1) : "}") + "]";
		}
		if(!Run(pEditor, Commands, Results, Error))
		{
			Request.m_Response = Rpc(Id, ToolError(Error, Results));
			return true;
		}
		if(Name == "editor_save")
		{
			auto Job = std::make_shared<CJob>();
			Job->m_Id = Id;
			Job->m_Save = true;
			Job->m_Path = String(&Arguments["path"]);
			auto Saved = Parse(Results[0]);
			Job->m_Resolved = String(&(*Saved)["path"]);
			Request.m_pUserState = Job;
			return false;
		}
		std::string Result = Results.empty() ? "null" : Results[0];
		if(Name == "editor_commands")
		{
			Result = "[";
			for(size_t i = 0; i < Results.size(); ++i)
				Result += (i ? "," : "") + Results[i];
			Result += "]";
		}
		Request.m_Response = Rpc(Id, TextResult(Result));
		return true;
	}
}

void UpdateEditorMcp(CEditor *pEditor)
{
	if(pEditor->m_pMcpServer && pEditor->m_pMcpServer->Running())
		pEditor->m_pMcpServer->Update([&](CEditorMcpServer::CRequest &Request) {
			if(Request.m_pUserState)
				return FinishJob(pEditor, Request);
			return EditorMcp::Dispatch(Request, [&](const std::string &Name, const json_value &Arguments, const std::string &Id, CEditorMcpServer::CRequest &Message) {
				return HandleTool(pEditor, Message, Name, Arguments, Id);
			});
		});
}
