#include "automation.h"

#include "editor.h"
#include "mapitems/image.h"
#include "mapitems/sound.h"

#include <base/io.h>

#include <engine/client.h>
#include <engine/console.h>
#include <engine/gfx/image_loader.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/shared/config.h>
#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/sound.h>
#include <engine/storage.h>

#include <algorithm>
#include <climits>
#include <set>

// Checkpoints retain resource objects and copy map geometry. They do not own
// another map or keep map tabs alive.
class CEditorAutomationState
{
public:
	struct CSnapshot
	{
		std::vector<std::shared_ptr<CLayerGroup>> m_vpGroups;
		std::vector<std::shared_ptr<CEditorImage>> m_vpImages;
		std::vector<int> m_vExternal;
		std::vector<std::string> m_vImageNames;
		std::vector<std::shared_ptr<CEditorSound>> m_vpSounds;
		std::vector<std::shared_ptr<CEnvelope>> m_vpEnvelopes;
		std::vector<CEditorMapSetting> m_vSettings;
		CEditorMap::CMapInfo m_Info;
	};
	std::map<std::string, CSnapshot> m_Checkpoints;
	std::deque<CSnapshot> m_Undo;
	std::deque<CSnapshot> m_Redo;
};

namespace
{
	constexpr int MAX_TILES = 4 * 1024 * 1024;

	// Validation is completed before each mutation. No exceptions are needed.
	class CRequest
	{
		const json_value &m_Value;
		std::set<std::string> m_Used;

	public:
		std::string m_Error;
		explicit CRequest(const json_value &Value) : m_Value(Value) {}
		void Fail(const char *pMessage)
		{
			if(m_Error.empty())
				m_Error = pMessage;
		}
		const json_value &Get(const char *pKey)
		{
			m_Used.insert(pKey);
			return m_Value[pKey];
		}
		int Int(const char *pKey, int Default, int Min = INT_MIN, int Max = INT_MAX)
		{
			const auto &Value = Get(pKey);
			if(Value.type == json_none)
				return Default;
			if(Value.type != json_integer || Value.u.integer < Min || Value.u.integer > Max)
			{
				Fail((std::string("Invalid integer: ") + pKey).c_str());
				return Default;
			}
			return Value.u.integer;
		}
		const char *Str(const char *pKey, const char *pDefault = "")
		{
			const auto &Value = Get(pKey);
			if(Value.type == json_none)
				return pDefault;
			if(Value.type != json_string || str_length(Value.u.string.ptr) != (int)Value.u.string.length)
			{
				Fail((std::string("Invalid string: ") + pKey).c_str());
				return pDefault;
			}
			return Value.u.string.ptr;
		}
		bool Finish()
		{
			if(m_Value.type != json_object)
				Fail("Expected an object");
			else
				for(unsigned i = 0; i < m_Value.u.object.length; ++i)
					if(!m_Used.count(m_Value.u.object.values[i].name))
						Fail((std::string("Unknown field: ") + m_Value.u.object.values[i].name).c_str());
			return m_Error.empty();
		}
	};

	void Int(CJsonWriter &W, const char *pName, int Value)
	{
		W.WriteAttribute(pName);
		W.WriteIntValue(Value);
	}
	void Str(CJsonWriter &W, const char *pName, const char *pValue)
	{
		W.WriteAttribute(pName);
		W.WriteStrValue(pValue);
	}

	const char *LayerKind(const CLayer *pLayer)
	{
		if(const auto *pTiles = dynamic_cast<const CLayerTiles *>(pLayer))
		{
			if(pTiles->m_HasGame)
				return "game";
			if(pTiles->m_HasFront)
				return "front";
			if(pTiles->m_HasTele)
				return "tele";
			if(pTiles->m_HasSpeedup)
				return "speedup";
			if(pTiles->m_HasSwitch)
				return "switch";
			if(pTiles->m_HasTune)
				return "tune";
			return "tiles";
		}
		return pLayer->m_Type == LAYERTYPE_QUADS ? "quads" : "sounds";
	}

	// The same property names are used for read and write. All coordinates in native
	// quad/source/envelope structures are integers in the map's fixed-point format.
	void QuadFields(CRequest *pR, CJsonWriter *pW, CQuad &Q, int EnvelopeCount)
	{
		for(int i = 0; i < 5; ++i)
		{
			const std::string X = "x" + std::to_string(i), Y = "y" + std::to_string(i);
			if(pR)
			{
				Q.m_aPoints[i].x = pR->Int(X.c_str(), Q.m_aPoints[i].x);
				Q.m_aPoints[i].y = pR->Int(Y.c_str(), Q.m_aPoints[i].y);
			}
			if(pW)
			{
				Int(*pW, X.c_str(), Q.m_aPoints[i].x);
				Int(*pW, Y.c_str(), Q.m_aPoints[i].y);
			}
		}
		for(int i = 0; i < 4; ++i)
		{
			const std::string U = "u" + std::to_string(i), V = "v" + std::to_string(i);
			if(pR)
			{
				Q.m_aTexcoords[i].x = pR->Int(U.c_str(), Q.m_aTexcoords[i].x);
				Q.m_aTexcoords[i].y = pR->Int(V.c_str(), Q.m_aTexcoords[i].y);
			}
			if(pW)
			{
				Int(*pW, U.c_str(), Q.m_aTexcoords[i].x);
				Int(*pW, V.c_str(), Q.m_aTexcoords[i].y);
			}
			int *apColors[] = {&Q.m_aColors[i].r, &Q.m_aColors[i].g, &Q.m_aColors[i].b, &Q.m_aColors[i].a};
			const char *apNames[] = {"r", "g", "b", "a"};
			for(int c = 0; c < 4; ++c)
			{
				const std::string Name = apNames[c] + std::to_string(i);
				if(pR)
					*apColors[c] = pR->Int(Name.c_str(), *apColors[c], 0, 255);
				if(pW)
					Int(*pW, Name.c_str(), *apColors[c]);
			}
		}
#define FIELD(name, member, min, max) \
	if(pR) \
		Q.member = pR->Int(name, Q.member, min, max); \
	if(pW) \
		Int(*pW, name, Q.member);
		FIELD("pos_env", m_PosEnv, -1, EnvelopeCount - 1)
		FIELD("pos_env_offset", m_PosEnvOffset, INT_MIN, INT_MAX)
		FIELD("color_env", m_ColorEnv, -1, EnvelopeCount - 1)
		FIELD("color_env_offset", m_ColorEnvOffset, INT_MIN, INT_MAX)
#undef FIELD
	}

	void SourceFields(CRequest *pR, CJsonWriter *pW, CSoundSource &S, int EnvelopeCount)
	{
#define FIELD(name, member, min, max) \
	if(pR) \
		S.member = pR->Int(name, S.member, min, max); \
	if(pW) \
		Int(*pW, name, S.member);
		FIELD("x", m_Position.x, INT_MIN, INT_MAX)
		FIELD("y", m_Position.y, INT_MIN, INT_MAX)
		FIELD("loop", m_Loop, 0, 1)
		FIELD("pan", m_Pan, 0, 1)
		FIELD("delay", m_TimeDelay, 0, INT_MAX)
		FIELD("falloff", m_Falloff, 0, 255)
		FIELD("pos_env", m_PosEnv, -1, EnvelopeCount - 1)
		FIELD("pos_env_offset", m_PosEnvOffset, INT_MIN, INT_MAX)
		FIELD("sound_env", m_SoundEnv, -1, EnvelopeCount - 1)
		FIELD("sound_env_offset", m_SoundEnvOffset, INT_MIN, INT_MAX)
		FIELD("shape", m_Shape.m_Type, 0, 1)
		if(S.m_Shape.m_Type == CSoundShape::SHAPE_CIRCLE)
		{
			FIELD("radius", m_Shape.m_Circle.m_Radius, 0, INT_MAX)
		}
		else
		{
			FIELD("width", m_Shape.m_Rectangle.m_Width, 0, INT_MAX)
			FIELD("height", m_Shape.m_Rectangle.m_Height, 0, INT_MAX)
		}
#undef FIELD
	}

	void TileFields(CRequest *pR, CJsonWriter *pW, CLayerTiles *pTiles, int Offset)
	{
		CTile Tile = pTiles->m_pTiles[Offset];
		int Index = Tile.m_Index, Flags = Tile.m_Flags;
		if(pR)
		{
			Index = pR->Int("index", 0, 0, 255);
			Flags = pR->Int("flags", 0, 0, 15);
		}
		if(pW)
		{
			Int(*pW, "index", Index);
			Int(*pW, "flags", Flags);
		}
#define SPECIAL(type, array, name, member, min, max) \
	if(auto *p = dynamic_cast<type *>(pTiles)) \
	{ \
		if(pR) \
			p->array[Offset].member = pR->Int(name, 0, min, max); \
		if(pW) \
			Int(*pW, name, p->array[Offset].member); \
	}
		SPECIAL(CLayerTele, m_pTeleTile, "number", m_Number, 0, 255)
		SPECIAL(CLayerTune, m_pTuneTile, "number", m_Number, 0, 255)
		SPECIAL(CLayerSwitch, m_pSwitchTile, "number", m_Number, 0, 255)
		SPECIAL(CLayerSwitch, m_pSwitchTile, "delay", m_Delay, 0, 255)
		SPECIAL(CLayerSpeedup, m_pSpeedupTile, "force", m_Force, 0, 255)
		SPECIAL(CLayerSpeedup, m_pSpeedupTile, "max_speed", m_MaxSpeed, 0, 255)
		SPECIAL(CLayerSpeedup, m_pSpeedupTile, "angle", m_Angle, -359, 359)
#undef SPECIAL
		if(pR)
		{
			pTiles->m_pTiles[Offset] = CTile{(unsigned char)Index, (unsigned char)Flags, 0, 0};
			if(auto *p = dynamic_cast<CLayerTele *>(pTiles))
				p->m_pTeleTile[Offset].m_Type = Index;
			if(auto *p = dynamic_cast<CLayerTune *>(pTiles))
				p->m_pTuneTile[Offset].m_Type = Index;
			if(auto *p = dynamic_cast<CLayerSpeedup *>(pTiles))
				p->m_pSpeedupTile[Offset].m_Type = Index;
			if(auto *p = dynamic_cast<CLayerSwitch *>(pTiles))
			{
				p->m_pSwitchTile[Offset].m_Type = Index;
				p->m_pSwitchTile[Offset].m_Flags = Flags;
			}
		}
	}

	template<typename T>
	void CopyRegion(T *pTarget, int TargetWidth, int X, int Y, const T *pSource, int SourceWidth, int SourceX, int SourceY, int Width, int Height)
	{
		for(int y = 0; y < Height; ++y)
			mem_copy(pTarget + (Y + y) * TargetWidth + X, pSource + (SourceY + y) * SourceWidth + SourceX, (size_t)Width * sizeof(T));
	}
	void CopyTiles(CLayerTiles *pTarget, int X, int Y, CLayerTiles *pSource, int SourceX, int SourceY, int Width, int Height)
	{
		CopyRegion(pTarget->m_pTiles, pTarget->m_Width, X, Y, pSource->m_pTiles, pSource->m_Width, SourceX, SourceY, Width, Height);
#define COPY(type, array) \
	if(auto *p = dynamic_cast<type *>(pTarget)) \
		CopyRegion(p->array, p->m_Width, X, Y, static_cast<type *>(pSource)->array, pSource->m_Width, SourceX, SourceY, Width, Height);
		COPY(CLayerTele, m_pTeleTile)
		COPY(CLayerTune, m_pTuneTile)
		COPY(CLayerSwitch, m_pSwitchTile)
		COPY(CLayerSpeedup, m_pSpeedupTile)
#undef COPY
	}
	void Inspect(CEditorMap *pMap, CJsonWriter &W)
	{
		W.BeginObject();
		Str(W, "filename", pMap->m_aFilename);
		Int(W, "modified", pMap->m_Modified);
		Int(W, "selected_group", pMap->m_SelectedGroup);
		Str(W, "author", pMap->m_MapInfo.m_aAuthor);
		Str(W, "version", pMap->m_MapInfo.m_aVersion);
		Str(W, "credits", pMap->m_MapInfo.m_aCredits);
		Str(W, "license", pMap->m_MapInfo.m_aLicense);
		W.WriteAttribute("settings");
		W.BeginArray();
		for(const auto &Setting : pMap->m_vSettings)
			W.WriteStrValue(Setting.m_aCommand);
		W.EndArray();
		W.WriteAttribute("images");
		W.BeginArray();
		for(const auto &p : pMap->m_vpImages)
		{
			W.BeginObject();
			Str(W, "name", p->m_aName);
			Int(W, "width", p->m_Width);
			Int(W, "height", p->m_Height);
			Int(W, "external", p->m_External);
			W.WriteAttribute("automapper_configs");
			W.BeginArray();
			for(int i = 0; i < p->m_Automapper.ConfigNamesNum(); ++i)
				W.WriteStrValue(p->m_Automapper.GetConfigName(i));
			W.EndArray();
			W.EndObject();
		}
		W.EndArray();
		W.WriteAttribute("sounds");
		W.BeginArray();
		for(const auto &p : pMap->m_vpSounds)
		{
			W.BeginObject();
			Str(W, "name", p->m_aName);
			Int(W, "bytes", p->m_DataSize);
			W.EndObject();
		}
		W.EndArray();
		W.WriteAttribute("envelopes");
		W.BeginArray();
		for(const auto &p : pMap->m_vpEnvelopes)
		{
			W.BeginObject();
			Str(W, "name", p->m_aName);
			Int(W, "channels", p->GetChannels());
			Int(W, "synchronized", p->m_Synchronized);
			W.WriteAttribute("points");
			W.BeginArray();
			for(const auto &Point : p->m_vPoints)
			{
				W.BeginObject();
				Int(W, "time", Point.m_Time.GetInternal());
				Int(W, "curve", Point.m_Curvetype);
				W.WriteAttribute("values");
				W.BeginArray();
				for(int c = 0; c < p->GetChannels(); ++c)
					W.WriteIntValue(Point.m_aValues[c]);
				W.EndArray();
				for(int c = 0; c < p->GetChannels(); ++c)
				{
					const std::string Suffix = std::to_string(c);
					Int(W, ("in_dx" + Suffix).c_str(), Point.m_Bezier.m_aInTangentDeltaX[c].GetInternal());
					Int(W, ("in_dy" + Suffix).c_str(), Point.m_Bezier.m_aInTangentDeltaY[c]);
					Int(W, ("out_dx" + Suffix).c_str(), Point.m_Bezier.m_aOutTangentDeltaX[c].GetInternal());
					Int(W, ("out_dy" + Suffix).c_str(), Point.m_Bezier.m_aOutTangentDeltaY[c]);
				}
				W.EndObject();
			}
			W.EndArray();
			W.EndObject();
		}
		W.EndArray();
		W.WriteAttribute("groups");
		W.BeginArray();
		for(const auto &pGroup : pMap->m_vpGroups)
		{
			W.BeginObject();
			Str(W, "name", pGroup->m_aName);
#define FIELD(name, member) Int(W, name, pGroup->member);
			FIELD("offset_x", m_OffsetX)
			FIELD("offset_y", m_OffsetY)
			FIELD("parallax_x", m_ParallaxX)
			FIELD("parallax_y", m_ParallaxY)
			FIELD("clipping", m_UseClipping)
			FIELD("clip_x", m_ClipX)
			FIELD("clip_y", m_ClipY)
			FIELD("clip_w", m_ClipW)
			FIELD("clip_h", m_ClipH)
			FIELD("visible", m_Visible)
#undef FIELD
			W.WriteAttribute("layers");
			W.BeginArray();
			for(const auto &pLayer : pGroup->m_vpLayers)
			{
				W.BeginObject();
				Str(W, "name", pLayer->m_aName);
				Str(W, "kind", LayerKind(pLayer.get()));
				Int(W, "flags", pLayer->m_Flags);
				Int(W, "visible", pLayer->m_Visible);
				Int(W, "readonly", pLayer->m_Readonly);
				if(auto *p = dynamic_cast<CLayerTiles *>(pLayer.get()))
				{
					Int(W, "width", p->m_Width);
					Int(W, "height", p->m_Height);
					Int(W, "image", p->m_Image);
					Int(W, "color_env", p->m_ColorEnv);
					Int(W, "color_env_offset", p->m_ColorEnvOffset);
					Int(W, "automapper_config", p->m_AutomapperConfig);
					Int(W, "automapper_reference", p->m_AutomapperReference);
					Int(W, "seed", p->m_Seed);
					Int(W, "auto_automapper", p->m_AutoAutomapper);
					Int(W, "overlays", p->m_RenderOverlays);
					Int(W, "r", p->m_Color.r);
					Int(W, "g", p->m_Color.g);
					Int(W, "b", p->m_Color.b);
					Int(W, "a", p->m_Color.a);
				}
				if(auto *p = dynamic_cast<CLayerQuads *>(pLayer.get()))
				{
					Int(W, "image", p->m_Image);
					W.WriteAttribute("quads");
					W.BeginArray();
					for(auto &Q : p->m_vQuads)
					{
						W.BeginObject();
						QuadFields(nullptr, &W, Q, pMap->m_vpEnvelopes.size());
						W.EndObject();
					}
					W.EndArray();
				}
				if(auto *p = dynamic_cast<CLayerSounds *>(pLayer.get()))
				{
					Int(W, "sound", p->m_Sound);
					W.WriteAttribute("sources");
					W.BeginArray();
					for(auto &S : p->m_vSources)
					{
						W.BeginObject();
						SourceFields(nullptr, &W, S, pMap->m_vpEnvelopes.size());
						W.EndObject();
					}
					W.EndArray();
				}
				W.EndObject();
			}
			W.EndArray();
			W.EndObject();
		}
		W.EndArray();
		W.EndObject();
	}

	void Modified(CEditorMap *pMap)
	{
		// Existing history actions hold layer indices/pointers which direct changes
		// can invalidate. Never leave stale native undo actions behind.
		pMap->m_EditorHistory.Clear();
		pMap->m_EnvelopeEditorHistory.Clear();
		pMap->m_ServerSettingsHistory.Clear();
		pMap->OnModify();
	}

	std::vector<std::shared_ptr<CLayerGroup>> CloneGroups(const std::vector<std::shared_ptr<CLayerGroup>> &vpGroups, CEditorMap *pMap)
	{
		std::vector<std::shared_ptr<CLayerGroup>> vpResult;
		for(const auto &pGroup : vpGroups)
		{
			auto pCopy = std::make_shared<CLayerGroup>(*pGroup);
			pCopy->m_vpLayers.clear();
			for(const auto &pLayer : pGroup->m_vpLayers)
				if(auto pGame = std::dynamic_pointer_cast<CLayerGame>(pLayer))
					pCopy->m_vpLayers.push_back(std::make_shared<CLayerGame>(*pGame));
				else
					pCopy->m_vpLayers.push_back(pLayer->Duplicate());
			pCopy->OnAttach(pMap);
			vpResult.push_back(pCopy);
		}
		return vpResult;
	}

	std::vector<std::shared_ptr<CEnvelope>> CloneEnvelopes(const std::vector<std::shared_ptr<CEnvelope>> &vpEnvelopes)
	{
		std::vector<std::shared_ptr<CEnvelope>> vpResult;
		for(const auto &p : vpEnvelopes)
		{
			auto pCopy = std::make_shared<CEnvelope>(p->GetChannels());
			pCopy->m_vPoints = p->m_vPoints;
			pCopy->m_Synchronized = p->m_Synchronized;
			str_copy(pCopy->m_aName, p->m_aName);
			vpResult.push_back(pCopy);
		}
		return vpResult;
	}

	CEditorAutomationState::CSnapshot CaptureSnapshot(CEditorMap *pMap)
	{
		CEditorAutomationState::CSnapshot Snapshot;
		Snapshot.m_vpGroups = CloneGroups(pMap->m_vpGroups, pMap);
		Snapshot.m_vpImages = pMap->m_vpImages;
		Snapshot.m_vpSounds = pMap->m_vpSounds;
		for(const auto &p : pMap->m_vpImages)
		{
			Snapshot.m_vExternal.push_back(p->m_External);
			Snapshot.m_vImageNames.emplace_back(p->m_aName);
		}
		Snapshot.m_vpEnvelopes = CloneEnvelopes(pMap->m_vpEnvelopes);
		Snapshot.m_vSettings = pMap->m_vSettings;
		Snapshot.m_Info.Copy(pMap->m_MapInfo);
		return Snapshot;
	}
	void RestoreSnapshot(CEditor *pEditor, const CEditorAutomationState::CSnapshot &Snapshot)
	{
		CEditorMap *pMap = pEditor->Map();
		pEditor->Reset();
		pMap->m_vpGroups = CloneGroups(Snapshot.m_vpGroups, pMap);
		pMap->m_vpImages = Snapshot.m_vpImages;
		pMap->m_vpSounds = Snapshot.m_vpSounds;
		for(size_t i = 0; i < pMap->m_vpImages.size(); ++i)
		{
			pMap->m_vpImages[i]->m_External = Snapshot.m_vExternal[i];
			str_copy(pMap->m_vpImages[i]->m_aName, Snapshot.m_vImageNames[i].c_str());
			pMap->m_vpImages[i]->m_Automapper.Unload();
			pMap->m_vpImages[i]->m_Automapper.Load(pMap->m_vpImages[i]->m_aName);
		}
		pMap->m_vpEnvelopes = CloneEnvelopes(Snapshot.m_vpEnvelopes);
		pMap->m_vSettings = Snapshot.m_vSettings;
		pMap->m_MapInfo.Copy(Snapshot.m_Info);
		pMap->m_MapInfoTmp.Copy(Snapshot.m_Info);
		pMap->m_pGameGroup.reset();
		pMap->m_pGameLayer.reset();
		pMap->m_pFrontLayer.reset();
		pMap->m_pTeleLayer.reset();
		pMap->m_pSpeedupLayer.reset();
		pMap->m_pSwitchLayer.reset();
		pMap->m_pTuneLayer.reset();
		for(const auto &pGroup : pMap->m_vpGroups)
		{
			if(pGroup->m_GameGroup)
				pMap->m_pGameGroup = pGroup;
			for(const auto &pLayer : pGroup->m_vpLayers)
			{
				if(auto p = std::dynamic_pointer_cast<CLayerTiles>(pLayer))
				{
					if(p->m_HasGame)
						pMap->MakeGameLayer(pLayer);
					if(p->m_HasFront)
						pMap->MakeFrontLayer(pLayer);
					if(p->m_HasTele)
						pMap->MakeTeleLayer(pLayer);
					if(p->m_HasSpeedup)
						pMap->MakeSpeedupLayer(pLayer);
					if(p->m_HasSwitch)
						pMap->MakeSwitchLayer(pLayer);
					if(p->m_HasTune)
						pMap->MakeTuneLayer(pLayer);
				}
			}
		}
		pMap->SelectGameLayer();
		pMap->DeselectQuads();
		pMap->DeselectQuadPoints();
		pMap->DeselectEnvPoints();
		pMap->m_SelectedImage = pMap->m_SelectedSound = 0;
		pMap->m_SelectedSoundSource = -1;

		pMap->m_SelectedEnvelope = 0;
		Modified(pMap);
	}
	bool ModifiesContent(const json_value &Value)
	{
		static const std::set<std::string> Operations = {"restore", "append", "tile_art", "quad_art", "brush_stamp", "tiles_text", "metadata", "settings", "group_add", "group_set", "group_delete", "group_move", "group_duplicate", "layer_add", "layer_set", "layer_delete", "layer_duplicate", "layer_move", "layer_resize", "layer_transform", "layer_shift", "tiles_set", "tiles_fill", "tiles_patch", "tiles_copy", "automap", "image_add", "image_replace", "image_set", "image_delete", "sound_add", "sound_replace", "sound_delete", "quad_add", "quad_set", "quad_delete", "quad_duplicate", "quad_move", "source_add", "source_set", "source_delete", "source_duplicate", "source_move", "envelope_add", "envelope_set", "envelope_delete", "envelope_duplicate", "envelope_move"};
		return Value["op"].type == json_string && Operations.count(Value["op"].u.string.ptr);
	}
	bool Execute(CEditor *pEditor, const json_value &Value, CJsonWriter &W, std::string &Error)
	{
		CRequest R(Value);
		const std::string Op = R.Str("op");
		CEditorMap *pMap = pEditor->Map();
		auto Finish = [&]() { if(!R.Finish()) { Error = R.m_Error; return false; } return true; };
		auto Done = [&]() { Modified(pMap); W.WriteNullValue(); return true; };
		if(Op == "undo" || Op == "redo")
		{
			if(!Finish())
				return false;
			if(!pMap->m_pAutomationState)
			{
				Error = "No automation history";
				return false;
			}
			auto &State = *pMap->m_pAutomationState;
			auto &From = Op == "undo" ? State.m_Undo : State.m_Redo;
			auto &To = Op == "undo" ? State.m_Redo : State.m_Undo;
			if(From.empty())
			{
				Error = "No automation history in that direction";
				return false;
			}
			To.push_back(CaptureSnapshot(pMap));
			auto Snapshot = std::move(From.back());
			From.pop_back();
			RestoreSnapshot(pEditor, Snapshot);
			W.WriteNullValue();
			return true;
		}
		if(Op == "constants")
		{
			if(!Finish())
				return false;
			W.BeginObject();
			Int(W, "LAYERTYPE_INVALID", LAYERTYPE_INVALID);
			Int(W, "LAYERTYPE_GAME", LAYERTYPE_GAME);
			Int(W, "LAYERTYPE_TILES", LAYERTYPE_TILES);
			Int(W, "LAYERTYPE_QUADS", LAYERTYPE_QUADS);
			Int(W, "LAYERTYPE_FRONT", LAYERTYPE_FRONT);
			Int(W, "LAYERTYPE_TELE", LAYERTYPE_TELE);
			Int(W, "LAYERTYPE_SPEEDUP", LAYERTYPE_SPEEDUP);
			Int(W, "LAYERTYPE_SWITCH", LAYERTYPE_SWITCH);
			Int(W, "LAYERTYPE_TUNE", LAYERTYPE_TUNE);
			Int(W, "LAYERTYPE_SOUNDS_DEPRECATED", LAYERTYPE_SOUNDS_DEPRECATED);
			Int(W, "LAYERTYPE_SOUNDS", LAYERTYPE_SOUNDS);
			Int(W, "CURVETYPE_STEP", CURVETYPE_STEP);
			Int(W, "CURVETYPE_LINEAR", CURVETYPE_LINEAR);
			Int(W, "CURVETYPE_SLOW", CURVETYPE_SLOW);
			Int(W, "CURVETYPE_FAST", CURVETYPE_FAST);
			Int(W, "CURVETYPE_SMOOTH", CURVETYPE_SMOOTH);
			Int(W, "CURVETYPE_BEZIER", CURVETYPE_BEZIER);
			Int(W, "ENTITY_NULL", ENTITY_NULL);
			Int(W, "ENTITY_SPAWN", ENTITY_SPAWN);
			Int(W, "ENTITY_SPAWN_RED", ENTITY_SPAWN_RED);
			Int(W, "ENTITY_SPAWN_BLUE", ENTITY_SPAWN_BLUE);
			Int(W, "ENTITY_FLAGSTAND_RED", ENTITY_FLAGSTAND_RED);
			Int(W, "ENTITY_FLAGSTAND_BLUE", ENTITY_FLAGSTAND_BLUE);
			Int(W, "ENTITY_ARMOR_1", ENTITY_ARMOR_1);
			Int(W, "ENTITY_HEALTH_1", ENTITY_HEALTH_1);
			Int(W, "ENTITY_WEAPON_SHOTGUN", ENTITY_WEAPON_SHOTGUN);
			Int(W, "ENTITY_WEAPON_GRENADE", ENTITY_WEAPON_GRENADE);
			Int(W, "ENTITY_POWERUP_NINJA", ENTITY_POWERUP_NINJA);
			Int(W, "ENTITY_WEAPON_LASER", ENTITY_WEAPON_LASER);
			Int(W, "ENTITY_LASER_FAST_CCW", ENTITY_LASER_FAST_CCW);
			Int(W, "ENTITY_LASER_NORMAL_CCW", ENTITY_LASER_NORMAL_CCW);
			Int(W, "ENTITY_LASER_SLOW_CCW", ENTITY_LASER_SLOW_CCW);
			Int(W, "ENTITY_LASER_STOP", ENTITY_LASER_STOP);
			Int(W, "ENTITY_LASER_SLOW_CW", ENTITY_LASER_SLOW_CW);
			Int(W, "ENTITY_LASER_NORMAL_CW", ENTITY_LASER_NORMAL_CW);
			Int(W, "ENTITY_LASER_FAST_CW", ENTITY_LASER_FAST_CW);
			Int(W, "ENTITY_LASER_SHORT", ENTITY_LASER_SHORT);
			Int(W, "ENTITY_LASER_MEDIUM", ENTITY_LASER_MEDIUM);
			Int(W, "ENTITY_LASER_LONG", ENTITY_LASER_LONG);
			Int(W, "ENTITY_LASER_C_SLOW", ENTITY_LASER_C_SLOW);
			Int(W, "ENTITY_LASER_C_NORMAL", ENTITY_LASER_C_NORMAL);
			Int(W, "ENTITY_LASER_C_FAST", ENTITY_LASER_C_FAST);
			Int(W, "ENTITY_LASER_O_SLOW", ENTITY_LASER_O_SLOW);
			Int(W, "ENTITY_LASER_O_NORMAL", ENTITY_LASER_O_NORMAL);
			Int(W, "ENTITY_LASER_O_FAST", ENTITY_LASER_O_FAST);
			Int(W, "ENTITY_PLASMAE", ENTITY_PLASMAE);
			Int(W, "ENTITY_PLASMAF", ENTITY_PLASMAF);
			Int(W, "ENTITY_PLASMA", ENTITY_PLASMA);
			Int(W, "ENTITY_PLASMAU", ENTITY_PLASMAU);
			Int(W, "ENTITY_CRAZY_SHOTGUN_EX", ENTITY_CRAZY_SHOTGUN_EX);
			Int(W, "ENTITY_CRAZY_SHOTGUN", ENTITY_CRAZY_SHOTGUN);
			Int(W, "ENTITY_ARMOR_SHOTGUN", ENTITY_ARMOR_SHOTGUN);
			Int(W, "ENTITY_ARMOR_GRENADE", ENTITY_ARMOR_GRENADE);
			Int(W, "ENTITY_ARMOR_NINJA", ENTITY_ARMOR_NINJA);
			Int(W, "ENTITY_ARMOR_LASER", ENTITY_ARMOR_LASER);
			Int(W, "ENTITY_DRAGGER_WEAK", ENTITY_DRAGGER_WEAK);
			Int(W, "ENTITY_DRAGGER_NORMAL", ENTITY_DRAGGER_NORMAL);
			Int(W, "ENTITY_DRAGGER_STRONG", ENTITY_DRAGGER_STRONG);
			Int(W, "ENTITY_DRAGGER_WEAK_NW", ENTITY_DRAGGER_WEAK_NW);
			Int(W, "ENTITY_DRAGGER_NORMAL_NW", ENTITY_DRAGGER_NORMAL_NW);
			Int(W, "ENTITY_DRAGGER_STRONG_NW", ENTITY_DRAGGER_STRONG_NW);
			Int(W, "ENTITY_DOOR", ENTITY_DOOR);
			Int(W, "ENTITY_OFFSET", ENTITY_OFFSET);
			Int(W, "TILE_AIR", TILE_AIR);
			Int(W, "TILE_SOLID", TILE_SOLID);
			Int(W, "TILE_DEATH", TILE_DEATH);
			Int(W, "TILE_NOHOOK", TILE_NOHOOK);
			Int(W, "TILE_NOLASER", TILE_NOLASER);
			Int(W, "TILE_THROUGH_CUT", TILE_THROUGH_CUT);
			Int(W, "TILE_THROUGH", TILE_THROUGH);
			Int(W, "TILE_JUMP", TILE_JUMP);
			Int(W, "TILE_FREEZE", TILE_FREEZE);
			Int(W, "TILE_TELEINEVIL", TILE_TELEINEVIL);
			Int(W, "TILE_UNFREEZE", TILE_UNFREEZE);
			Int(W, "TILE_DFREEZE", TILE_DFREEZE);
			Int(W, "TILE_DUNFREEZE", TILE_DUNFREEZE);
			Int(W, "TILE_TELEINWEAPON", TILE_TELEINWEAPON);
			Int(W, "TILE_TELEINHOOK", TILE_TELEINHOOK);
			Int(W, "TILE_WALLJUMP", TILE_WALLJUMP);
			Int(W, "TILE_EHOOK_ENABLE", TILE_EHOOK_ENABLE);
			Int(W, "TILE_EHOOK_DISABLE", TILE_EHOOK_DISABLE);
			Int(W, "TILE_HIT_ENABLE", TILE_HIT_ENABLE);
			Int(W, "TILE_HIT_DISABLE", TILE_HIT_DISABLE);
			Int(W, "TILE_SOLO_ENABLE", TILE_SOLO_ENABLE);
			Int(W, "TILE_SOLO_DISABLE", TILE_SOLO_DISABLE);
			Int(W, "TILE_SWITCHTIMEDOPEN", TILE_SWITCHTIMEDOPEN);
			Int(W, "TILE_SWITCHTIMEDCLOSE", TILE_SWITCHTIMEDCLOSE);
			Int(W, "TILE_SWITCHOPEN", TILE_SWITCHOPEN);
			Int(W, "TILE_SWITCHCLOSE", TILE_SWITCHCLOSE);
			Int(W, "TILE_TELEIN", TILE_TELEIN);
			Int(W, "TILE_TELEOUT", TILE_TELEOUT);
			Int(W, "TILE_SPEED_BOOST_OLD", TILE_SPEED_BOOST_OLD);
			Int(W, "TILE_SPEED_BOOST", TILE_SPEED_BOOST);
			Int(W, "TILE_TELECHECK", TILE_TELECHECK);
			Int(W, "TILE_TELECHECKOUT", TILE_TELECHECKOUT);
			Int(W, "TILE_TELECHECKIN", TILE_TELECHECKIN);
			Int(W, "TILE_REFILL_JUMPS", TILE_REFILL_JUMPS);
			Int(W, "TILE_START", TILE_START);
			Int(W, "TILE_FINISH", TILE_FINISH);
			Int(W, "TILE_TIME_CHECKPOINT_FIRST", TILE_TIME_CHECKPOINT_FIRST);
			Int(W, "TILE_TIME_CHECKPOINT_LAST", TILE_TIME_CHECKPOINT_LAST);
			Int(W, "TILE_STOP", TILE_STOP);
			Int(W, "TILE_STOPS", TILE_STOPS);
			Int(W, "TILE_STOPA", TILE_STOPA);
			Int(W, "TILE_TELECHECKINEVIL", TILE_TELECHECKINEVIL);
			Int(W, "TILE_CP", TILE_CP);
			Int(W, "TILE_CP_F", TILE_CP_F);
			Int(W, "TILE_THROUGH_ALL", TILE_THROUGH_ALL);
			Int(W, "TILE_THROUGH_DIR", TILE_THROUGH_DIR);
			Int(W, "TILE_TUNE", TILE_TUNE);
			Int(W, "TILE_OLDLASER", TILE_OLDLASER);
			Int(W, "TILE_NPC", TILE_NPC);
			Int(W, "TILE_EHOOK", TILE_EHOOK);
			Int(W, "TILE_NOHIT", TILE_NOHIT);
			Int(W, "TILE_NPH", TILE_NPH);
			Int(W, "TILE_UNLOCK_TEAM", TILE_UNLOCK_TEAM);
			Int(W, "TILE_ADD_TIME", TILE_ADD_TIME);
			Int(W, "TILE_NPC_DISABLE", TILE_NPC_DISABLE);
			Int(W, "TILE_UNLIMITED_JUMPS_DISABLE", TILE_UNLIMITED_JUMPS_DISABLE);
			Int(W, "TILE_JETPACK_DISABLE", TILE_JETPACK_DISABLE);
			Int(W, "TILE_NPH_DISABLE", TILE_NPH_DISABLE);
			Int(W, "TILE_SUBTRACT_TIME", TILE_SUBTRACT_TIME);
			Int(W, "TILE_TELE_GUN_ENABLE", TILE_TELE_GUN_ENABLE);
			Int(W, "TILE_TELE_GUN_DISABLE", TILE_TELE_GUN_DISABLE);
			Int(W, "TILE_ALLOW_TELE_GUN", TILE_ALLOW_TELE_GUN);
			Int(W, "TILE_ALLOW_BLUE_TELE_GUN", TILE_ALLOW_BLUE_TELE_GUN);
			Int(W, "TILE_NPC_ENABLE", TILE_NPC_ENABLE);
			Int(W, "TILE_UNLIMITED_JUMPS_ENABLE", TILE_UNLIMITED_JUMPS_ENABLE);
			Int(W, "TILE_JETPACK_ENABLE", TILE_JETPACK_ENABLE);
			Int(W, "TILE_NPH_ENABLE", TILE_NPH_ENABLE);
			Int(W, "TILE_TELE_GRENADE_ENABLE", TILE_TELE_GRENADE_ENABLE);
			Int(W, "TILE_TELE_GRENADE_DISABLE", TILE_TELE_GRENADE_DISABLE);
			Int(W, "TILE_TELE_LASER_ENABLE", TILE_TELE_LASER_ENABLE);
			Int(W, "TILE_TELE_LASER_DISABLE", TILE_TELE_LASER_DISABLE);
			Int(W, "TILE_CREDITS_1", TILE_CREDITS_1);
			Int(W, "TILE_CREDITS_2", TILE_CREDITS_2);
			Int(W, "TILE_CREDITS_3", TILE_CREDITS_3);
			Int(W, "TILE_CREDITS_4", TILE_CREDITS_4);
			Int(W, "TILE_LFREEZE", TILE_LFREEZE);
			Int(W, "TILE_LUNFREEZE", TILE_LUNFREEZE);
			Int(W, "TILE_CREDITS_5", TILE_CREDITS_5);
			Int(W, "TILE_CREDITS_6", TILE_CREDITS_6);
			Int(W, "TILE_CREDITS_7", TILE_CREDITS_7);
			Int(W, "TILE_CREDITS_8", TILE_CREDITS_8);
			Int(W, "TILE_ENTITIES_OFF_1", TILE_ENTITIES_OFF_1);
			Int(W, "TILE_ENTITIES_OFF_2", TILE_ENTITIES_OFF_2);
			Int(W, "TILEFLAG_XFLIP", TILEFLAG_XFLIP);
			Int(W, "TILEFLAG_YFLIP", TILEFLAG_YFLIP);
			Int(W, "TILEFLAG_OPAQUE", TILEFLAG_OPAQUE);
			Int(W, "TILEFLAG_ROTATE", TILEFLAG_ROTATE);
			Int(W, "LAYERFLAG_DETAIL", LAYERFLAG_DETAIL);
			W.EndObject();
			return true;
		}
		if(Op == "dialog_close")
		{
			if(!Finish())
				return false;
			pEditor->Reset();
			pEditor->OnDialogClose();
			pEditor->m_PopupEventActivated = false;
			W.WriteNullValue();
			return true;
		}
		if(Op == "ui")
		{
			int Mode = R.Int("mode", pEditor->m_Mode, 0, NUM_MODES - 1);
			int Gui = R.Int("gui", pEditor->m_GuiActive, 0, 1);
			int Detail = R.Int("detail", pMap->m_ShowDetail, 0, 1);
			int Info = R.Int("tile_info", pEditor->m_ShowTileInfo, 0, 2);
			int Grid = R.Int("grid", pEditor->MapView()->MapGrid()->IsEnabled(), 0, 1);
			int Factor = R.Int("grid_factor", pEditor->MapView()->MapGrid()->Factor(), 1, 15);
			int Preview = R.Int("envelope_preview", pEditor->m_ShowEnvelopePreview, 0, 1);
			int Extra = R.Int("extra", pEditor->m_ActiveExtraEditor, -1, CEditor::NUM_EXTRAEDITORS - 1);
			if(!Finish())
				return false;
			pEditor->m_Mode = Mode;
			pEditor->m_GuiActive = Gui;
			pMap->m_ShowDetail = Detail;
			pEditor->m_ShowTileInfo = (CEditor::EShowTile)Info;
			if((bool)Grid != pEditor->MapView()->MapGrid()->IsEnabled())
				pEditor->MapView()->MapGrid()->Toggle();
			pEditor->MapView()->MapGrid()->SetFactor(Factor);
			pEditor->m_ShowEnvelopePreview = Preview;
			pEditor->m_ActiveExtraEditor = (CEditor::EExtraEditor)Extra;
			W.WriteNullValue();
			return true;
		}
		if(Op == "console_commands" || Op == "console")
		{
			auto *pConsole = pEditor->Console();
			const char *pCommand = Op == "console" ? R.Str("command") : "";
			if(Op == "console" && !pCommand[0])
				R.Fail("command is required");
			if(!Finish())
				return false;
			if(Op == "console")
			{
				pConsole->ExecuteLineFlag(pCommand, CFGFLAG_CLIENT, -1);
				W.WriteNullValue();
				return true;
			}
			W.BeginArray();
			for(auto *p = pConsole->FirstCommandInfo(-1, CFGFLAG_CLIENT); p; p = pConsole->NextCommandInfo(p, -1, CFGFLAG_CLIENT))
			{
				W.BeginObject();
				Str(W, "name", p->Name());
				Str(W, "parameters", p->Params());
				Str(W, "help", p->Help());
				W.EndObject();
			}
			W.EndArray();
			return true;
		}
		if(Op == "tile_art" || Op == "quad_art")
		{
			const char *pPath = R.Str("path");
			CQuadArtParameters Parameters{};
			str_copy(Parameters.m_aFilename, pPath);
			if(Op == "quad_art")
			{
				Parameters.m_ImagePixelSize = R.Int("image_pixel_size", 1, 1, 1024);
				Parameters.m_QuadPixelSize = R.Int("quad_pixel_size", 32, 1, 100000);
				Parameters.m_Centralize = R.Int("centralize", 0, 0, 1);
				Parameters.m_Optimize = R.Int("optimize", 1, 0, 1);
			}
			if(!pPath[0])
				R.Fail("path is required");
			if(!Finish())
				return false;
			CImageInfo Image;
			if(!pEditor->Graphics()->LoadPng(Image, pPath, IStorage::TYPE_ALL_OR_ABSOLUTE))
			{
				Error = "Failed to load art PNG";
				return false;
			}
			if((int64_t)Image.m_Width * Image.m_Height > MAX_TILES)
			{
				Image.Free();
				Error = "Art exceeds pixel limit";
				return false;
			}
			ConvertToRgba(Image);
			if(Op == "tile_art")
			{
				std::set<uint32_t> Colors;
				for(size_t i = 0; i < (size_t)Image.m_Width * Image.m_Height; ++i)
					if(Image.m_pData[i * 4 + 3])
					{
						uint32_t Color;
						mem_copy(&Color, Image.m_pData + i * 4, 4);
						Colors.insert(Color);
					}
				if(pMap->m_vpImages.size() + (Colors.size() + 254) / 255 > MAX_MAPIMAGES)
				{
					Image.Free();
					Error = "Art has too many colors for map image slots";
					return false;
				}
				pMap->AddTileArt(std::move(Image), pPath, true);
			}
			else
				pMap->AddQuadArt(std::move(Image), Parameters, true);
			Modified(pMap);
			W.WriteIntValue(pMap->m_vpGroups.size() - 1);
			return true;
		}
		if(Op == "brush_clear" || Op == "brush_transform" || Op == "brush_info")
		{
			const char *pTransform = Op == "brush_transform" ? R.Str("transform") : "";
			if(Op == "brush_transform" && str_comp(pTransform, "flip_x") && str_comp(pTransform, "flip_y") && str_comp(pTransform, "rotate_cw") && str_comp(pTransform, "rotate_ccw"))
				R.Fail("Unknown brush transform");
			if(!Finish())
				return false;
			if(Op == "brush_info")
			{
				W.BeginArray();
				for(const auto &pLayer : pEditor->m_pBrush->m_vpLayers)
				{
					W.BeginObject();
					Str(W, "kind", LayerKind(pLayer.get()));
					if(auto *p = dynamic_cast<CLayerTiles *>(pLayer.get()))
					{
						Int(W, "width", p->m_Width);
						Int(W, "height", p->m_Height);
						Int(W, "image", p->m_Image);
					}
					if(auto *p = dynamic_cast<CLayerQuads *>(pLayer.get()))
						Int(W, "quads", p->m_vQuads.size());
					if(auto *p = dynamic_cast<CLayerSounds *>(pLayer.get()))
						Int(W, "sources", p->m_vSources.size());
					W.EndObject();
				}
				W.EndArray();
				return true;
			}
			if(Op == "brush_clear")
				pEditor->m_pBrush->Clear();
			else
				for(const auto &p : pEditor->m_pBrush->m_vpLayers)
				{
					if(!str_comp(pTransform, "flip_x"))
						p->BrushFlipX();
					else if(!str_comp(pTransform, "flip_y"))
						p->BrushFlipY();
					else
						p->BrushRotate(!str_comp(pTransform, "rotate_cw") ? pi / 2 : -pi / 2);
				}
			W.WriteNullValue();
			return true;
		}
		if(Op == "image_add" || Op == "image_replace" || Op == "sound_add" || Op == "sound_replace")
		{
			const bool Image = Op == "image_add" || Op == "image_replace";
			const bool Replace = Op == "image_replace" || Op == "sound_replace";
			const char *pPath = R.Str("path");
			int Id = Replace ? R.Int(Image ? "image" : "sound", -1, 0, (int)(Image ? pMap->m_vpImages.size() : pMap->m_vpSounds.size()) - 1) : -1;
			char aDefaultName[IO_MAX_PATH_LENGTH];
			fs_split_file_extension(fs_filename(pPath), aDefaultName, sizeof(aDefaultName));
			const char *pName = R.Str("name", aDefaultName);
			if(str_length(pName) >= IO_MAX_PATH_LENGTH)
				R.Fail("Asset name is too long");
			if(!pPath[0] || !pName[0])
				R.Fail("path and asset name must not be empty");
			if(Replace && Id < 0)
				R.Fail("Resource index is required");
			int External = Image ? R.Int("external", 0, 0, 1) : 0;
			if(Image)
				for(int i = 0; i < (int)pMap->m_vpImages.size(); ++i)
				{
					if(i != Id && !str_comp(pMap->m_vpImages[i]->m_aName, pName))
						R.Fail("Image name already exists");
				}
			else
				for(int i = 0; i < (int)pMap->m_vpSounds.size(); ++i)
				{
					if(i != Id && !str_comp(pMap->m_vpSounds[i]->m_aName, pName))
						R.Fail("Sound name already exists");
				}
			if(!Replace && (Image ? pMap->m_vpImages.size() >= MAX_MAPIMAGES : pMap->m_vpSounds.size() >= MAX_MAPSOUNDS))
				R.Fail("Map resource limit reached");
			if(!Finish())
				return false;
			if(Image)
			{
				CImageInfo Info;
				if(!pEditor->Graphics()->LoadPng(Info, pPath, IStorage::TYPE_ALL_OR_ABSOLUTE))
				{
					Error = "Failed to load PNG";
					return false;
				}
				auto pImage = std::make_shared<CEditorImage>(pMap);
				*pImage = std::move(Info);
				str_copy(pImage->m_aName, pName);
				pImage->m_External = External;
				ConvertToRgba(*pImage);
				DilateImage(*pImage);
				int Flags = pImage->m_Width % 16 == 0 && pImage->m_Height % 16 == 0 ? pEditor->Graphics()->TextureLoadFlags() : 0;
				pImage->m_Texture = pEditor->Graphics()->LoadTextureRaw(*pImage, Flags, pPath);
				pImage->AnalyseTileFlags();
				pImage->m_Automapper.Load(pName);
				if(Replace)
					pMap->m_vpImages[Id] = pImage;
				else
					pMap->m_vpImages.push_back(pImage);
				pMap->SortImages();
				pMap->SelectImage(pImage);
				Id = pMap->m_SelectedImage;
			}
			else
			{
				void *pData;
				unsigned Size;
				if(!pEditor->Storage()->ReadFile(pPath, IStorage::TYPE_ALL_OR_ABSOLUTE, &pData, &Size))
				{
					Error = "Failed to read sound";
					return false;
				}
				int Sample = pEditor->Sound()->LoadOpusFromMem(pData, Size, true, pPath);
				if(Sample < 0)
				{
					free(pData);
					Error = "Failed to decode Opus sound";
					return false;
				}
				auto pSound = std::make_shared<CEditorSound>(pMap);
				pSound->m_SoundId = Sample;
				pSound->m_pData = pData;
				pSound->m_DataSize = Size;
				str_copy(pSound->m_aName, pName);
				if(Replace)
					pMap->m_vpSounds[Id] = pSound;
				else
				{
					pMap->m_vpSounds.push_back(pSound);
					Id = pMap->m_vpSounds.size() - 1;
				}
				pMap->SelectSound(pSound);
			}
			Modified(pMap);
			W.WriteIntValue(Id);
			return true;
		}
		if(Op == "image_export" || Op == "sound_export" || Op == "sound_play" || Op == "sound_stop")
		{
			bool Image = Op == "image_export";
			int Id = R.Int(Image ? "image" : "sound", -1, 0, (int)(Image ? pMap->m_vpImages.size() : pMap->m_vpSounds.size()) - 1);
			bool Export = Op == "image_export" || Op == "sound_export";
			const char *pPath = Export ? R.Str("path") : "";
			int Loop = Op == "sound_play" ? R.Int("loop", 0, 0, 1) : 0;
			if(Id < 0 || (Export && !pPath[0]))
				R.Fail("Resource index and export path are required");
			if(!Finish())
				return false;
			if(!Export)
			{
				int Sample = pMap->m_vpSounds[Id]->m_SoundId;
				if(Op == "sound_stop")
					pEditor->Sound()->Stop(Sample);
				else
					pEditor->Sound()->Play(0, Sample, Loop ? ISound::FLAG_LOOP : 0, 1.0f);
				W.WriteNullValue();
				return true;
			}
			char aResolved[IO_MAX_PATH_LENGTH];
			IOHANDLE File = pEditor->Storage()->OpenFile(pPath, IOFLAG_WRITE, IStorage::TYPE_SAVE_OR_ABSOLUTE, aResolved, sizeof(aResolved));
			if(!File)
			{
				Error = "Cannot open asset export path";
				return false;
			}
			bool Success;
			if(Image)
				Success = CImageLoader::SavePng(File, pPath, *pMap->m_vpImages[Id]);
			else
			{
				auto pSound = pMap->m_vpSounds[Id];
				Success = io_write(File, pSound->m_pData, pSound->m_DataSize) == pSound->m_DataSize;
				io_close(File);
			}
			if(!Success)
			{
				Error = "Asset export failed";
				return false;
			}
			W.BeginObject();
			Str(W, "path", aResolved);
			W.EndObject();
			return true;
		}
		if(Op == "tabs" || Op == "tab_select" || Op == "tab_close")
		{
			int Tab = Op == "tabs" ? 0 : R.Int("tab", pEditor->SelectedMapIndex(), 0, pEditor->MapCount() - 1);
			if(!Finish())
				return false;
			if(Op == "tabs")
			{
				W.BeginArray();
				for(size_t i = 0; i < pEditor->MapCount(); ++i)
				{
					W.BeginObject();
					Int(W, "tab", i);
					Str(W, "filename", pEditor->MapAt(i)->m_aFilename);
					Int(W, "modified", pEditor->MapAt(i)->m_Modified);
					Int(W, "selected", i == pEditor->SelectedMapIndex());
					W.EndObject();
				}
				W.EndArray();
				return true;
			}
			if(Op == "tab_close")
			{
				if(pEditor->IsSaving(pEditor->MapAt(Tab)->m_aFilename))
				{
					Error = "Map is still saving";
					return false;
				}
				pEditor->CloseMap(Tab, false);
			}
			else
				pEditor->SelectMap(Tab);
			W.WriteNullValue();
			return true;
		}
		if(Op == "actions" || Op == "action")
		{
			const char *pAction = Op == "action" ? R.Str("name") : "";
			if(!Finish())
				return false;
			if(Op == "actions")
				W.BeginArray();
#define REGISTER_QUICK_ACTION(name, text, callback, disabled, active, button_color, description) \
	if(Op == "actions") \
	{ \
		W.BeginObject(); \
		Str(W, "name", #name); \
		Str(W, "description", pEditor->m_QuickAction##name.Description()); \
		Int(W, "disabled", pEditor->m_QuickAction##name.Disabled()); \
		W.EndObject(); \
	} \
	else if(!str_comp(pAction, #name)) \
	{ \
		if(pEditor->m_QuickAction##name.Disabled()) \
		{ \
			Error = "Editor action is disabled"; \
			return false; \
		} \
		pEditor->m_QuickAction##name.Call(); \
		W.WriteNullValue(); \
		return true; \
	}
#include "quick_actions.h"
#undef REGISTER_QUICK_ACTION
			if(Op == "actions")
			{
				W.EndArray();
				return true;
			}
			Error = "Unknown editor action";
			return false;
		}
		if(Op == "status")
		{
			if(!Finish())
				return false;
			W.BeginObject();
			Int(W, "saving", !pEditor->m_WriterFinishJobs.empty());
			Int(W, "dialog", pEditor->m_Dialog);
			Int(W, "modified", pMap->m_Modified);
			Int(W, "screen_width", pEditor->Graphics()->ScreenWidth());
			Int(W, "screen_height", pEditor->Graphics()->ScreenHeight());
			Int(W, "window_active", pEditor->EngineGraphics()->WindowActive());
			Int(W, "window_open", pEditor->EngineGraphics()->WindowOpen());
			Int(W, "camera_x", pEditor->MapView()->GetWorldOffset().x);
			Int(W, "camera_y", pEditor->MapView()->GetWorldOffset().y);
			Int(W, "zoom", pEditor->MapView()->Zoom()->GetValue());
			Int(W, "mode", pEditor->m_Mode);
			Int(W, "selected_tab", pEditor->SelectedMapIndex());
			Int(W, "undo_steps", pMap->m_pAutomationState ? pMap->m_pAutomationState->m_Undo.size() : 0);
			Int(W, "redo_steps", pMap->m_pAutomationState ? pMap->m_pAutomationState->m_Redo.size() : 0);
			W.EndObject();
			return true;
		}
		if(Op == "checkpoint" || Op == "restore" || Op == "checkpoint_delete")
		{
			const char *pName = R.Str("name", "default");
			if(!Finish())
				return false;
			if(!pMap->m_pAutomationState)
				pMap->m_pAutomationState = std::make_shared<CEditorAutomationState>();
			auto &Checkpoints = pMap->m_pAutomationState->m_Checkpoints;
			if(Op == "checkpoint")
			{
				if(Checkpoints.size() >= 8 && !Checkpoints.count(pName))
				{
					Error = "At most 8 checkpoints per map";
					return false;
				}
				auto Snapshot = CaptureSnapshot(pMap);
				Checkpoints[pName] = std::move(Snapshot);
				W.WriteNullValue();
				return true;
			}
			auto It = Checkpoints.find(pName);
			if(It == Checkpoints.end())
			{
				Error = "Unknown checkpoint";
				return false;
			}
			if(Op == "checkpoint_delete")
			{
				Checkpoints.erase(It);
				W.WriteNullValue();
				return true;
			}
			RestoreSnapshot(pEditor, It->second);
			return Done();
		}
		if(Op == "inspect")
		{
			if(!Finish())
				return false;
			Inspect(pMap, W);
			return true;
		}
		if(Op == "new")
		{
			if(!Finish())
				return false;
			pEditor->AddDefaultMap();
			W.WriteNullValue();
			return true;
		}
		if(Op == "save_status")
		{
			const char *pPath = R.Str("path");
			if(!pPath[0])
				R.Fail("path is required");
			if(!Finish())
				return false;
			const auto It = pEditor->m_AutomationSaveResults.find(pPath);
			const bool Saving = pEditor->IsSaving(pPath);
			W.BeginObject();
			Int(W, "saving", Saving);
			Int(W, "finished", !Saving && It != pEditor->m_AutomationSaveResults.end());
			Str(W, "error", It == pEditor->m_AutomationSaveResults.end() ? "" : It->second.c_str());
			W.EndObject();
			return true;
		}
		if(Op == "load" || Op == "save" || Op == "append" || Op == "screenshot")
		{
			const char *pPath = R.Str("path");
			if(!pPath[0])
				R.Fail("path is required");
			if(!Finish())
				return false;
			bool Success = true;
			if(Op == "load")
				Success = pEditor->Load(pPath, IStorage::TYPE_ALL_OR_ABSOLUTE);
			else if(Op == "save")
			{
				Success = pEditor->Save(pPath);
				if(Success)
				{
					if(pEditor->m_AutomationSaveResults.size() >= 32)
						pEditor->m_AutomationSaveResults.erase(pEditor->m_AutomationSaveResults.begin());
					pEditor->m_AutomationSaveResults[pPath] = "";
					str_copy(pMap->m_aFilename, pPath);
					pMap->m_ValidSaveFilename = true;
					pMap->m_Modified = false;
					pEditor->UpdateMapDisplayNames();
				}
			}
			else if(Op == "append")
				Success = pMap->Append(pPath, IStorage::TYPE_ALL_OR_ABSOLUTE, true, [&](const char *pMessage) { Error = pMessage; });
			else
				pEditor->Graphics()->TakeCustomScreenshot(pPath);
			if(!Success)
			{
				if(Error.empty())
					Error = "File operation failed; check the editor log";
				return false;
			}
			if(Op == "append")
				Modified(pMap);
			if(Op == "screenshot" || Op == "save")
			{
				char aResolved[IO_MAX_PATH_LENGTH];
				pEditor->Storage()->GetCompletePath(IStorage::TYPE_SAVE, pPath, aResolved, sizeof(aResolved));
				W.BeginObject();
				Str(W, "path", aResolved);
				if(Op == "screenshot")
				{
					CUIRect View = pEditor->MapView()->LastViewport();
					const auto *pScreen = pEditor->Ui()->Screen();
					float SX = pEditor->Graphics()->ScreenWidth() / pScreen->w, SY = pEditor->Graphics()->ScreenHeight() / pScreen->h;
					Int(W, "viewport_x", View.x * SX);
					Int(W, "viewport_y", View.y * SY);
					Int(W, "viewport_width", View.w * SX);
					Int(W, "viewport_height", View.h * SY);
					Int(W, "camera_x", pEditor->MapView()->GetWorldOffset().x);
					Int(W, "camera_y", pEditor->MapView()->GetWorldOffset().y);
					Int(W, "zoom", pEditor->MapView()->Zoom()->GetValue());
				}
				W.EndObject();
			}
			else
				W.WriteNullValue();
			return true;
		}
		if(Op == "view_fit")
		{
			int X = R.Int("x", 0, -10000000, 10000000), Y = R.Int("y", 0, -10000000, 10000000);
			int Width = R.Int("width", pMap->m_pGameLayer->m_Width * 32, 1, 10000000), Height = R.Int("height", pMap->m_pGameLayer->m_Height * 32, 1, 10000000);
			int Padding = R.Int("padding", 10, 0, 100);
			if(!Finish())
				return false;
			CUIRect View = pEditor->MapView()->LastViewport();
			const auto *pScreen = pEditor->Ui()->Screen();
			if(View.w <= 0 || View.h <= 0)
			{
				Error = "Editor viewport has not rendered yet";
				return false;
			}
			CScreenRect Base = pEditor->Graphics()->MapScreenToWorld(0, 0, 100, 100, 100, 0, 0, pEditor->Graphics()->ScreenAspect(), 1.0f);
			float Zoom = std::max(Width / (Base.Width() * View.w / pScreen->w), Height / (Base.Height() * View.h / pScreen->h)) * (1.0f + Padding / 100.0f);
			Zoom = std::clamp(Zoom, 0.1f, 20.0f);
			vec2 Center(X + Width / 2.0f, Y + Height / 2.0f);
			vec2 Shift(((View.x + View.w / 2.0f) / pScreen->w - 0.5f) * Base.Width() * Zoom, ((View.y + View.h / 2.0f) / pScreen->h - 0.5f) * Base.Height() * Zoom);
			pEditor->MapView()->SetEditorOffset({0, 0});
			pEditor->MapView()->SetWorldOffset(Center - Shift);
			pEditor->MapView()->Zoom()->SetValueInstant(Zoom * 100);
			W.WriteNullValue();
			return true;
		}
		if(Op == "view")
		{
			int X = R.Int("x", pMap->m_MapViewState.m_WorldOffset.x, -10000000, 10000000);
			int Y = R.Int("y", pMap->m_MapViewState.m_WorldOffset.y, -10000000, 10000000);
			int Zoom = R.Int("zoom", pEditor->MapView()->Zoom()->GetValue(), 10, 2000);
			if(!Finish())
				return false;
			pEditor->MapView()->SetWorldOffset(vec2(X, Y));
			pEditor->MapView()->Zoom()->SetValueInstant(Zoom);
			W.WriteNullValue();
			return true;
		}
		if(Op == "metadata")
		{
			// Omitted fields default to the current value. Copy them first: str_copy clears its destination
			// before reading the source, so copying a buffer onto itself would erase it.
			const std::string Author = R.Str("author", pMap->m_MapInfo.m_aAuthor), Version = R.Str("version", pMap->m_MapInfo.m_aVersion);
			const std::string Credits = R.Str("credits", pMap->m_MapInfo.m_aCredits), License = R.Str("license", pMap->m_MapInfo.m_aLicense);
			if(!Finish())
				return false;
			str_copy(pMap->m_MapInfo.m_aAuthor, Author.c_str());
			str_copy(pMap->m_MapInfo.m_aVersion, Version.c_str());
			str_copy(pMap->m_MapInfo.m_aCredits, Credits.c_str());
			str_copy(pMap->m_MapInfo.m_aLicense, License.c_str());
			pMap->m_MapInfoTmp.Copy(pMap->m_MapInfo);
			return Done();
		}
		if(Op == "settings")
		{
			const auto &Commands = R.Get("commands");
			std::vector<CEditorMapSetting> vSettings;
			if(Commands.type != json_array)
				R.Fail("commands must be an array of strings");
			else
				for(unsigned i = 0; i < Commands.u.array.length; ++i)
				{
					if(Commands[i].type != json_string || Commands[i].u.string.length >= 256)
						R.Fail("Invalid server setting");
					else
						vSettings.emplace_back(Commands[i].u.string.ptr);
				}
			if(!Finish())
				return false;
			pMap->m_vSettings = std::move(vSettings);
			return Done();
		}
		if(Op == "group_add")
		{
			const char *pName = R.Str("name");
			if(!Finish())
				return false;
			auto p = pMap->NewGroup();
			str_copy(p->m_aName, pName);
			Modified(pMap);
			W.WriteIntValue(pMap->m_vpGroups.size() - 1);
			return true;
		}
		if(Op == "envelope_duplicate" || Op == "envelope_move")
		{
			int Id = R.Int("envelope", -1, 0, (int)pMap->m_vpEnvelopes.size() - 1);
			int To = Op == "envelope_move" ? R.Int("to", 0, 0, (int)pMap->m_vpEnvelopes.size() - 1) : 0;
			if(Id < 0)
				R.Fail("envelope is required");
			if(!Finish())
				return false;
			if(Op == "envelope_move")
				Id = pMap->MoveEnvelope(Id, To);
			else
			{
				auto vp = CloneEnvelopes({pMap->m_vpEnvelopes[Id]});
				pMap->m_vpEnvelopes.push_back(vp[0]);
				Id = pMap->m_vpEnvelopes.size() - 1;
			}
			Modified(pMap);
			W.WriteIntValue(Id);
			return true;
		}
		if(Op == "envelope_add" || Op == "envelope_set" || Op == "envelope_delete")
		{
			int Id = Op == "envelope_add" ? -1 : R.Int("envelope", -1, 0, pMap->m_vpEnvelopes.size() - 1);
			if(Op != "envelope_add" && Id < 0)
				R.Fail("envelope is required");
			if(Op == "envelope_delete")
			{
				if(!Finish())
					return false;
				pMap->DeleteEnvelope(Id);
				return Done();
			}
			int Channels = Op == "envelope_add" ? R.Int("channels", 4, 1, 4) : (Id >= 0 ? pMap->m_vpEnvelopes[Id]->GetChannels() : 4);
			if(Channels == 2)
				R.Fail("channels must be 1, 3 or 4");
			auto pEnv = std::make_shared<CEnvelope>(Channels == 2 ? 4 : Channels);
			if(Id >= 0)
			{
				pEnv->m_vPoints = pMap->m_vpEnvelopes[Id]->m_vPoints;
				pEnv->m_Synchronized = pMap->m_vpEnvelopes[Id]->m_Synchronized;
				str_copy(pEnv->m_aName, pMap->m_vpEnvelopes[Id]->m_aName);
			}
			const std::string Name = R.Str("name", pEnv->m_aName); // owned copy, see metadata
			pEnv->m_Synchronized = R.Int("synchronized", pEnv->m_Synchronized, 0, 1);
			const auto &Points = R.Get("points");
			if(Points.type != json_none)
			{
				pEnv->m_vPoints.clear();
				if(Points.type != json_array)
					R.Fail("points must be an array");
				else
					for(unsigned i = 0; i < Points.u.array.length; ++i)
					{
						CRequest P(Points[i]);
						CEnvPoint_runtime Point{};
						Point.m_Time = CFixedTime(P.Int("time", 0, 0));
						Point.m_Curvetype = P.Int("curve", CURVETYPE_LINEAR, 0, CURVETYPE_BEZIER);
						const auto &Values = P.Get("values");
						if(Values.type != json_array || Values.u.array.length != (unsigned)Channels)
							P.Fail("values must match envelope channels");
						else
							for(int c = 0; c < Channels; ++c)
							{
								if(Values[c].type != json_integer || Values[c].u.integer < INT_MIN || Values[c].u.integer > INT_MAX)
									P.Fail("Invalid envelope value");
								else
									Point.m_aValues[c] = Values[c].u.integer;
							}
						for(int c = 0; c < Channels; ++c)
						{
							const std::string Suffix = std::to_string(c);
							Point.m_Bezier.m_aInTangentDeltaX[c] = CFixedTime(P.Int(("in_dx" + Suffix).c_str(), 0));
							Point.m_Bezier.m_aInTangentDeltaY[c] = P.Int(("in_dy" + Suffix).c_str(), 0);
							Point.m_Bezier.m_aOutTangentDeltaX[c] = CFixedTime(P.Int(("out_dx" + Suffix).c_str(), 0));
							Point.m_Bezier.m_aOutTangentDeltaY[c] = P.Int(("out_dy" + Suffix).c_str(), 0);
						}
						if(!P.Finish())
							R.Fail(P.m_Error.c_str());
						pEnv->m_vPoints.push_back(Point);
					}
				std::sort(pEnv->m_vPoints.begin(), pEnv->m_vPoints.end());
				for(size_t i = 1; i < pEnv->m_vPoints.size(); ++i)
					if(pEnv->m_vPoints[i - 1].m_Time == pEnv->m_vPoints[i].m_Time)
						R.Fail("Envelope point times must be unique");
			}
			if(!Finish())
				return false;
			str_copy(pEnv->m_aName, Name.c_str());
			if(Id < 0)
			{
				pMap->m_vpEnvelopes.push_back(pEnv);
				Id = pMap->m_vpEnvelopes.size() - 1;
			}
			else
			{
				auto p = pMap->m_vpEnvelopes[Id];
				p->m_vPoints = std::move(pEnv->m_vPoints);
				p->m_Synchronized = pEnv->m_Synchronized;
				str_copy(p->m_aName, pEnv->m_aName);
			}
			Modified(pMap);
			W.WriteIntValue(Id);
			return true;
		}
		if(Op == "image_set" || Op == "image_delete" || Op == "sound_delete")
		{
			const bool Image = Op != "sound_delete";
			int Id = R.Int(Image ? "image" : "sound", -1, 0, (Image ? pMap->m_vpImages.size() : pMap->m_vpSounds.size()) - 1);
			if(Id < 0)
				R.Fail("Resource index is required");
			if(Op == "image_set")
			{
				int External = R.Int("external", Id >= 0 ? pMap->m_vpImages[Id]->m_External : 0, 0, 1);
				const std::string Name = R.Str("name", Id >= 0 ? pMap->m_vpImages[Id]->m_aName : "");
				const char *pName = Name.c_str();
				if(!pName[0] || str_length(pName) >= IO_MAX_PATH_LENGTH)
					R.Fail("Invalid image name length");
				for(int i = 0; i < (int)pMap->m_vpImages.size(); ++i)
					if(i != Id && !str_comp(pName, pMap->m_vpImages[i]->m_aName))
						R.Fail("Image name already exists");
				if(!Finish())
					return false;
				pMap->m_vpImages[Id]->m_External = External;
				str_copy(pMap->m_vpImages[Id]->m_aName, pName);
				pMap->m_vpImages[Id]->m_Automapper.Unload();
				pMap->m_vpImages[Id]->m_Automapper.Load(pName);
				pMap->SortImages();
				return Done();
			}
			if(!Finish())
				return false;
			auto Remap = [Id](int *pIndex) { if(*pIndex == Id) *pIndex = -1; else if(*pIndex > Id) --*pIndex; };
			if(Image)
			{
				pMap->ModifyImageIndex(Remap);
				pMap->m_vpImages.erase(pMap->m_vpImages.begin() + Id);
				pMap->m_SelectedImage = 0;
			}
			else
			{
				pMap->ModifySoundIndex(Remap);
				pMap->m_vpSounds.erase(pMap->m_vpSounds.begin() + Id);
				pMap->m_SelectedSound = 0;
			}
			return Done();
		}

		int Group = R.Int("group", -1, 0, pMap->m_vpGroups.size() - 1);
		if(Group < 0)
			R.Fail("group is required");
		if(!R.m_Error.empty())
		{
			Error = R.m_Error;
			return false;
		}
		auto pGroup = pMap->m_vpGroups[Group];
		if(Op == "group_set")
		{
			// Work on a copy until all fields have been checked.
			CLayerGroup Copy(*pGroup);
			const std::string Name = R.Str("name", Copy.m_aName); // owned copy, see metadata
#define FIELD(name, member, min, max) Copy.member = R.Int(name, Copy.member, min, max);
			FIELD("offset_x", m_OffsetX, -10000000, 10000000)
			FIELD("offset_y", m_OffsetY, -10000000, 10000000)
			FIELD("parallax_x", m_ParallaxX, -100000, 100000)
			FIELD("parallax_y", m_ParallaxY, -100000, 100000)
			FIELD("clipping", m_UseClipping, 0, 1)
			FIELD("clip_x", m_ClipX, -10000000, 10000000)
			FIELD("clip_y", m_ClipY, -10000000, 10000000)
			FIELD("clip_w", m_ClipW, 0, 10000000)
			FIELD("clip_h", m_ClipH, 0, 10000000)
			FIELD("visible", m_Visible, 0, 1)
#undef FIELD
			if(pGroup->m_GameGroup && (Copy.m_OffsetX || Copy.m_OffsetY || Copy.m_ParallaxX != 100 || Copy.m_ParallaxY != 100 || Copy.m_UseClipping))
				R.Fail("Game group transform is fixed");
			if(!Finish())
				return false;
			str_copy(Copy.m_aName, Name.c_str());
			pGroup->m_OffsetX = Copy.m_OffsetX;
			pGroup->m_OffsetY = Copy.m_OffsetY;
			pGroup->m_ParallaxX = Copy.m_ParallaxX;
			pGroup->m_ParallaxY = Copy.m_ParallaxY;
			pGroup->m_UseClipping = Copy.m_UseClipping;
			pGroup->m_ClipX = Copy.m_ClipX;
			pGroup->m_ClipY = Copy.m_ClipY;
			pGroup->m_ClipW = Copy.m_ClipW;
			pGroup->m_ClipH = Copy.m_ClipH;
			pGroup->m_Visible = Copy.m_Visible;
			str_copy(pGroup->m_aName, Copy.m_aName);
			return Done();
		}
		if(Op == "group_delete" || Op == "group_move" || Op == "group_duplicate")
		{
			int To = Op == "group_move" ? R.Int("to", Group, 0, pMap->m_vpGroups.size() - 1) : 0;
			if(Op != "group_move" && pGroup->m_GameGroup)
				R.Fail("Cannot delete the game group");
			if(!Finish())
				return false;
			if(Op == "group_move")
				pMap->MoveGroup(Group, To);
			else if(Op == "group_duplicate")
			{
				auto vpClones = CloneGroups({pGroup}, pMap);
				pMap->m_vpGroups.insert(pMap->m_vpGroups.begin() + Group + 1, vpClones[0]);
				pMap->SelectGameLayer();
				Modified(pMap);
				W.WriteIntValue(Group + 1);
				return true;
			}
			else
				pMap->DeleteGroup(Group);
			pMap->SelectGameLayer();
			return Done();
		}
		if(Op == "layer_add")
		{
			std::string Kind = R.Str("kind", "tiles");
			const char *pName = R.Str("name", Kind.c_str());
			int Width = R.Int("width", pMap->m_pGameLayer->m_Width, 1, MAX_TILES), Height = R.Int("height", pMap->m_pGameLayer->m_Height, 1, MAX_TILES);
			if((int64_t)Width * Height > MAX_TILES)
				R.Fail("Layer exceeds tile limit");
			bool Special = Kind == "front" || Kind == "tele" || Kind == "speedup" || Kind == "switch" || Kind == "tune";
			if(Special && (!pGroup->m_GameGroup || Width != pMap->m_pGameLayer->m_Width || Height != pMap->m_pGameLayer->m_Height))
				R.Fail("Entity layers must match the game layer dimensions and group");
			if((Kind == "front" && pMap->m_pFrontLayer) || (Kind == "tele" && pMap->m_pTeleLayer) || (Kind == "speedup" && pMap->m_pSpeedupLayer) || (Kind == "switch" && pMap->m_pSwitchLayer) || (Kind == "tune" && pMap->m_pTuneLayer))
				R.Fail("Entity layer already exists");
			if(!Special && Kind != "tiles" && Kind != "quads" && Kind != "sounds")
				R.Fail("Unknown layer kind; edit the existing game layer for game tiles");
			if(!Finish())
				return false;
			std::shared_ptr<CLayer> p;
			if(Kind == "tiles")
				p = std::make_shared<CLayerTiles>(pMap, Width, Height);
			else if(Kind == "quads")
				p = std::make_shared<CLayerQuads>(pMap);
			else if(Kind == "sounds")
				p = std::make_shared<CLayerSounds>(pMap);
			else if(Kind == "front")
			{
				p = std::make_shared<CLayerFront>(pMap, Width, Height);
				pMap->MakeFrontLayer(p);
			}
			else if(Kind == "tele")
			{
				p = std::make_shared<CLayerTele>(pMap, Width, Height);
				pMap->MakeTeleLayer(p);
			}
			else if(Kind == "speedup")
			{
				p = std::make_shared<CLayerSpeedup>(pMap, Width, Height);
				pMap->MakeSpeedupLayer(p);
			}
			else if(Kind == "switch")
			{
				p = std::make_shared<CLayerSwitch>(pMap, Width, Height);
				pMap->MakeSwitchLayer(p);
			}
			else
			{
				p = std::make_shared<CLayerTune>(pMap, Width, Height);
				pMap->MakeTuneLayer(p);
			}
			str_copy(p->m_aName, pName);
			pGroup->AddLayer(p);
			Modified(pMap);
			W.WriteIntValue(pGroup->m_vpLayers.size() - 1);
			return true;
		}
		int Layer = R.Int("layer", -1, 0, pGroup->m_vpLayers.size() - 1);
		if(Layer < 0)
			R.Fail("layer is required");
		if(!R.m_Error.empty())
		{
			Error = R.m_Error;
			return false;
		}
		auto pLayer = pGroup->m_vpLayers[Layer];
		auto *pTiles = dynamic_cast<CLayerTiles *>(pLayer.get());
		auto *pQuads = dynamic_cast<CLayerQuads *>(pLayer.get());
		auto *pSounds = dynamic_cast<CLayerSounds *>(pLayer.get());
		if(Op == "brush_grab" || Op == "brush_stamp")
		{
			int X = R.Int("x", 0, -10000000, 10000000), Y = R.Int("y", 0, -10000000, 10000000);
			int Width = Op == "brush_grab" ? R.Int("width", 32, 1, 10000000) : 0, Height = Op == "brush_grab" ? R.Int("height", 32, 1, 10000000) : 0;
			int BrushLayer = Op == "brush_stamp" ? R.Int("brush_layer", 0, 0, (int)pEditor->m_pBrush->m_vpLayers.size() - 1) : 0;
			int Destructive = Op == "brush_stamp" ? R.Int("destructive", 1, 0, 1) : 1;
			if(Op == "brush_stamp" && (pEditor->m_pBrush->m_vpLayers.empty() || pLayer->m_Readonly))
				R.Fail("Brush is empty or destination is read-only");
			if(!Finish())
				return false;
			if(Op == "brush_grab")
			{
				pEditor->m_pBrush->Clear();
				CUIRect Rect;
				Rect.x = X;
				Rect.y = Y;
				Rect.w = Width;
				Rect.h = Height;
				int Count = pLayer->BrushGrab(pEditor->m_pBrush.get(), Rect);
				W.WriteIntValue(Count);
				return true;
			}
			auto pBrushLayer = pEditor->m_pBrush->m_vpLayers[BrushLayer];
			if(pBrushLayer->m_Type != pLayer->m_Type || (pTiles && str_comp(LayerKind(pLayer.get()), LayerKind(pBrushLayer.get()))))
			{
				Error = "Brush kind does not match destination layer";
				return false;
			}
			bool PreviousDestructive = pEditor->m_BrushDrawDestructive;
			pEditor->m_BrushDrawDestructive = Destructive;
			if(pTiles)
				pLayer->BrushDraw(pBrushLayer.get(), vec2(X, Y));
			else
				pLayer->BrushPlace(pBrushLayer.get(), vec2(X, Y));
			pEditor->m_BrushDrawDestructive = PreviousDestructive;
			return Done();
		}
		if(Op == "tiles_text")
		{
			if(!pTiles || pTiles->IsEntitiesLayer())
			{
				Error = "Text requires a decorative font tile layer";
				return false;
			}
			const char *pText = R.Str("text");
			int X = R.Int("x", 0, 0, pTiles->m_Width - 1), Y = R.Int("y", 0, 0, pTiles->m_Height - 1);
			int CursorX = X, CursorY = Y;
			auto pClone = pTiles->Duplicate();
			auto *pScratch = static_cast<CLayerTiles *>(pClone.get());
			for(const char *p = pText; *p; ++p)
			{
				if(*p == '\r')
					continue;
				if(*p == '\n')
				{
					CursorX = X;
					++CursorY;
					continue;
				}
				char Letter = str_uppercase(*p);
				int Index = Letter >= 'A' && Letter <= 'Z' ? Letter - 'A' + 1 : Letter >= '1' && Letter <= '9' ? Letter - '1' + 54 :
											Letter == '0'                          ? 63 :
											Letter == ' '                          ? 0 :
																 -1;
				if(Index < 0)
				{
					R.Fail("Font text supports ASCII letters, digits, spaces and newlines");
					break;
				}
				if(CursorX >= pTiles->m_Width || CursorY >= pTiles->m_Height)
				{
					R.Fail("Text does not fit in layer bounds");
					break;
				}
				pScratch->m_pTiles[CursorY * pTiles->m_Width + CursorX++] = CTile{(unsigned char)Index, 0, 0, 0};
			}
			if(!Finish())
				return false;
			CopyTiles(pTiles, 0, 0, pScratch, 0, 0, pTiles->m_Width, pTiles->m_Height);
			return Done();
		}
		if(Op == "selection")
		{
			std::vector<int> vLayers{Layer}, vQuads, vPoints;
			auto Indices = [&](const char *pKey, std::vector<int> &vResult, int Max) {
				const auto &Array = R.Get(pKey);
				if(Array.type == json_none)
					return;
				vResult.clear();
				if(Array.type != json_array)
				{
					R.Fail("Selection must be an array");
					return;
				}
				for(unsigned i = 0; i < Array.u.array.length; ++i)
				{
					if(Array[i].type != json_integer || Array[i].u.integer < 0 || Array[i].u.integer >= Max)
						R.Fail("Selection index out of range");
					else if(std::find(vResult.begin(), vResult.end(), Array[i].u.integer) == vResult.end())
						vResult.push_back(Array[i].u.integer);
				}
			};
			Indices("layers", vLayers, pGroup->m_vpLayers.size());
			Indices("quads", vQuads, pQuads ? pQuads->m_vQuads.size() : 0);
			Indices("quad_points", vPoints, 5);
			if(vLayers.empty() || vLayers.front() != Layer)
				R.Fail("Selected layers must start with the layer used for object selection");
			int Source = R.Int("source", -1, -1, pSounds ? (int)pSounds->m_vSources.size() - 1 : -1);
			int Envelope = R.Int("envelope", pMap->m_SelectedEnvelope, -1, (int)pMap->m_vpEnvelopes.size() - 1);
			if(!Finish())
				return false;
			pMap->SelectLayer(Layer, Group);
			pMap->m_vSelectedLayers = std::move(vLayers);
			pMap->DeselectQuads();
			pMap->DeselectQuadPoints();
			pMap->m_vSelectedQuads = std::move(vQuads);
			for(int Id : vPoints)
				pMap->m_SelectedQuadPoints |= 1 << Id;
			pMap->m_SelectedSoundSource = Source;
			pMap->m_SelectedEnvelope = Envelope;
			W.WriteNullValue();
			return true;
		}
		if(Op == "tiles_patch" || Op == "tiles_copy" || Op == "layer_shift")
		{
			if(!pTiles)
			{
				Error = "Expected a tile layer";
				return false;
			}
			auto pClone = pTiles->Duplicate();
			auto *pScratch = static_cast<CLayerTiles *>(pClone.get());
			if(Op == "tiles_patch")
			{
				const auto &Tiles = R.Get("tiles");
				if(Tiles.type != json_array || Tiles.u.array.length > 100000)
					R.Fail("tiles must be an array of at most 100000 entries");
				else
					for(unsigned i = 0; i < Tiles.u.array.length; ++i)
					{
						CRequest Cell(Tiles[i]);
						int X = Cell.Int("x", -1, 0, pTiles->m_Width - 1), Y = Cell.Int("y", -1, 0, pTiles->m_Height - 1);
						if(X < 0 || Y < 0)
							Cell.Fail("Tile x and y are required");
						if(Cell.m_Error.empty())
							TileFields(&Cell, nullptr, pScratch, Y * pScratch->m_Width + X);
						if(!Cell.Finish())
						{
							R.Fail(Cell.m_Error.c_str());
							break;
						}
					}
			}
			else if(Op == "tiles_copy")
			{
				int SourceGroup = R.Int("source_group", Group, 0, (int)pMap->m_vpGroups.size() - 1);
				int SourceLayer = R.Int("source_layer", Layer, 0, (int)pMap->m_vpGroups[SourceGroup]->m_vpLayers.size() - 1);
				if(SourceLayer < 0 || SourceLayer >= (int)pMap->m_vpGroups[SourceGroup]->m_vpLayers.size())
					R.Fail("Source layer is out of range");
				if(!R.m_Error.empty())
				{
					Error = R.m_Error;
					return false;
				}
				auto *pSource = dynamic_cast<CLayerTiles *>(pMap->m_vpGroups[SourceGroup]->m_vpLayers[SourceLayer].get());
				if(!pSource || str_comp(LayerKind(pSource), LayerKind(pTiles)))
				{
					Error = "Source and destination tile kinds must match";
					return false;
				}
				int X = R.Int("x", 0, 0, pTiles->m_Width - 1), Y = R.Int("y", 0, 0, pTiles->m_Height - 1);
				int SX = R.Int("source_x", 0, 0, pSource->m_Width - 1), SY = R.Int("source_y", 0, 0, pSource->m_Height - 1);
				int Width = R.Int("width", 1, 1, std::min(pTiles->m_Width - X, pSource->m_Width - SX)), Height = R.Int("height", 1, 1, std::min(pTiles->m_Height - Y, pSource->m_Height - SY));
				if(R.m_Error.empty())
					CopyTiles(pScratch, X, Y, pSource, SX, SY, Width, Height);
			}
			else
			{
				int DX = R.Int("dx", 0, -pTiles->m_Width, pTiles->m_Width), DY = R.Int("dy", 0, -pTiles->m_Height, pTiles->m_Height);
				mem_zero(pScratch->m_pTiles, (size_t)pScratch->m_Width * pScratch->m_Height * sizeof(CTile));
#define CLEAR(type, array, item) \
	if(auto *p = dynamic_cast<type *>(pScratch)) \
		mem_zero(p->array, (size_t)p->m_Width * p->m_Height * sizeof(item));
				CLEAR(CLayerTele, m_pTeleTile, CTeleTile)
				CLEAR(CLayerTune, m_pTuneTile, CTuneTile)
				CLEAR(CLayerSwitch, m_pSwitchTile, CSwitchTile)
				CLEAR(CLayerSpeedup, m_pSpeedupTile, CSpeedupTile)
#undef CLEAR
				if(R.m_Error.empty())
					CopyTiles(pScratch, std::max(0, DX), std::max(0, DY), pTiles, std::max(0, -DX), std::max(0, -DY), pTiles->m_Width - std::abs(DX), pTiles->m_Height - std::abs(DY));
			}
			if(!Finish())
				return false;
			CopyTiles(pTiles, 0, 0, pScratch, 0, 0, pTiles->m_Width, pTiles->m_Height);
			return Done();
		}
		if(Op == "quad_duplicate" || Op == "quad_move" || Op == "source_duplicate" || Op == "source_move")
		{
			bool Quad = Op == "quad_duplicate" || Op == "quad_move";
			bool Move = Op == "quad_move" || Op == "source_move";
			if((Quad && !pQuads) || (!Quad && !pSounds))
			{
				Error = "Object kind does not match layer";
				return false;
			}
			int Count = Quad ? pQuads->m_vQuads.size() : pSounds->m_vSources.size();
			int Id = R.Int(Quad ? "quad" : "source", -1, 0, Count - 1), To = Move ? R.Int("to", 0, 0, Count - 1) : Count;
			if(Id < 0)
				R.Fail("Object index is required");
			if(!Finish())
				return false;
			if(Quad)
			{
				CQuad Copy = pQuads->m_vQuads[Id];
				if(Move)
					pQuads->m_vQuads.erase(pQuads->m_vQuads.begin() + Id);
				pQuads->m_vQuads.insert(pQuads->m_vQuads.begin() + To, Copy);
				pMap->DeselectQuads();
				pMap->DeselectQuadPoints();
			}
			else
			{
				CSoundSource Copy = pSounds->m_vSources[Id];
				if(Move)
					pSounds->m_vSources.erase(pSounds->m_vSources.begin() + Id);
				pSounds->m_vSources.insert(pSounds->m_vSources.begin() + To, Copy);
				pMap->m_SelectedSoundSource = -1;
			}
			Modified(pMap);
			W.WriteIntValue(To);
			return true;
		}
		if(Op == "select")
		{
			if(!Finish())
				return false;
			pMap->SelectLayer(Layer, Group);
			W.WriteNullValue();
			return true;
		}
		if(Op == "layer_delete" || Op == "layer_duplicate" || Op == "layer_move")
		{
			int ToGroup = Op == "layer_move" ? R.Int("to_group", Group, 0, pMap->m_vpGroups.size() - 1) : Group;
			int To = Op == "layer_move" ? R.Int("to", ToGroup == Group ? Layer : (int)pMap->m_vpGroups[ToGroup]->m_vpLayers.size(), 0, (int)pMap->m_vpGroups[ToGroup]->m_vpLayers.size() - (ToGroup == Group ? 1 : 0)) : 0;
			if(ToGroup != Group && pLayer->IsEntitiesLayer())
				R.Fail("Entity layers must remain in the game group");
			if(pLayer == pMap->m_pGameLayer && Op != "layer_move")
				R.Fail("Cannot delete or duplicate the game layer");
			if(pLayer->IsEntitiesLayer() && Op == "layer_duplicate")
				R.Fail("Cannot duplicate an entity layer");
			if(!Finish())
				return false;
			if(Op == "layer_move")
			{
				if(ToGroup == Group)
					pGroup->MoveLayer(Layer, To);
				else
				{
					pGroup->m_vpLayers.erase(pGroup->m_vpLayers.begin() + Layer);
					auto pTarget = pMap->m_vpGroups[ToGroup];
					pTarget->m_vpLayers.insert(pTarget->m_vpLayers.begin() + To, pLayer);
				}
			}
			else if(Op == "layer_duplicate")
				pGroup->DuplicateLayer(Layer);
			else
			{
				if(pLayer == pMap->m_pFrontLayer)
					pMap->m_pFrontLayer.reset();
				if(pLayer == pMap->m_pTeleLayer)
					pMap->m_pTeleLayer.reset();
				if(pLayer == pMap->m_pSpeedupLayer)
					pMap->m_pSpeedupLayer.reset();
				if(pLayer == pMap->m_pSwitchLayer)
					pMap->m_pSwitchLayer.reset();
				if(pLayer == pMap->m_pTuneLayer)
					pMap->m_pTuneLayer.reset();
				pGroup->DeleteLayer(Layer);
			}
			pMap->SelectGameLayer();
			return Done();
		}
		if(Op == "layer_set")
		{
			const std::string Name = R.Str("name", pLayer->m_aName); // owned copy, see metadata
			int Flags = R.Int("flags", pLayer->m_Flags, 0, LAYERFLAG_DETAIL), Visible = R.Int("visible", pLayer->m_Visible, 0, 1), Readonly = R.Int("readonly", pLayer->m_Readonly, 0, 1);
			int Image = -1, Sound = -1, Env = -1, Offset = 0;
			int AutoConfig = -1, Reference = -1, Seed = 0, Auto = 0, Overlays = 1;
			CColor Color{};
			if(pTiles || pQuads)
				Image = R.Int("image", pTiles ? pTiles->m_Image : pQuads->m_Image, -1, pMap->m_vpImages.size() - 1);
			if(pSounds)
				Sound = R.Int("sound", pSounds->m_Sound, -1, pMap->m_vpSounds.size() - 1);
			if(pTiles)
			{
				if(pTiles->IsEntitiesLayer() && Image != -1)
					R.Fail("Entity layers cannot use images");
				Color.r = R.Int("r", pTiles->m_Color.r, 0, 255);
				Color.g = R.Int("g", pTiles->m_Color.g, 0, 255);
				Color.b = R.Int("b", pTiles->m_Color.b, 0, 255);
				Color.a = R.Int("a", pTiles->m_Color.a, 0, 255);
				Env = R.Int("color_env", pTiles->m_ColorEnv, -1, pMap->m_vpEnvelopes.size() - 1);
				Offset = R.Int("color_env_offset", pTiles->m_ColorEnvOffset);
				AutoConfig = R.Int("automapper_config", Image == pTiles->m_Image ? pTiles->m_AutomapperConfig : -1, -1, Image >= 0 ? (int)pMap->m_vpImages[Image]->m_Automapper.ConfigNamesNum() - 1 : -1);
				Reference = R.Int("automapper_reference", pTiles->m_AutomapperReference, -1, 9);
				Seed = R.Int("seed", pTiles->m_Seed, 0);
				Auto = R.Int("auto_automapper", pTiles->m_AutoAutomapper, 0, 1);
				Overlays = R.Int("overlays", pTiles->m_RenderOverlays, 0, 1);
			}
			if(!Finish())
				return false;
			str_copy(pLayer->m_aName, Name.c_str());
			pLayer->m_Flags = Flags;
			pLayer->m_Visible = Visible;
			pLayer->m_Readonly = Readonly;
			if(pTiles)
			{
				pTiles->m_Image = Image;
				pTiles->m_Color = Color;
				pTiles->m_ColorEnv = Env;
				pTiles->m_ColorEnvOffset = Offset;
				pTiles->m_AutomapperConfig = AutoConfig;
				pTiles->m_AutomapperReference = Reference;
				pTiles->m_Seed = Seed;
				pTiles->m_AutoAutomapper = Auto;
				pTiles->m_RenderOverlays = Overlays;
			}
			if(pQuads)
				pQuads->m_Image = Image;
			if(pSounds)
				pSounds->m_Sound = Sound;
			return Done();
		}
		if(Op == "automap")
		{
			if(!pTiles || pTiles->IsEntitiesLayer() || pTiles->m_Image < 0)
			{
				Error = "Automapping requires a decorative tile layer with an image";
				return false;
			}
			auto &Automapper = pMap->m_vpImages[pTiles->m_Image]->m_Automapper;
			int Config = R.Int("config", 0, 0, Automapper.ConfigNamesNum() - 1);
			int Seed = R.Int("seed", 1, 1);
			int Reference = R.Int("reference", -1, -1, 9);
			if(!Automapper.IsLoaded() || Automapper.ConfigNamesNum() == 0)
				R.Fail("Image has no automapper rules");
			if(!Finish())
				return false;
			Automapper.Proceed(pTiles, pMap->m_pGameLayer.get(), Reference, Config, Seed);
			return Done();
		}
		if(Op == "tiles_get" || Op == "tiles_fill" || Op == "tiles_set" || Op == "layer_resize" || Op == "layer_transform")
		{
			if(!pTiles)
			{
				Error = "Expected a tile layer";
				return false;
			}
			if(Op == "layer_resize")
			{
				int Width = R.Int("width", pTiles->m_Width, 1, MAX_TILES), Height = R.Int("height", pTiles->m_Height, 1, MAX_TILES);
				if((int64_t)Width * Height > MAX_TILES)
					R.Fail("Layer exceeds tile limit");
				if(pTiles->IsEntitiesLayer() && pTiles != pMap->m_pGameLayer.get())
					R.Fail("Resize the game layer to resize entity layers together");
				if(!Finish())
					return false;
				pTiles->Resize(Width, Height);
				return Done();
			}
			if(Op == "layer_transform")
			{
				std::string Transform = R.Str("transform");
				if(Transform != "flip_x" && Transform != "flip_y" && Transform != "rotate_cw" && Transform != "rotate_ccw")
					R.Fail("Unknown transform");
				if(!Finish())
					return false;
				if(Transform == "flip_x")
					pTiles->BrushFlipX();
				else if(Transform == "flip_y")
					pTiles->BrushFlipY();
				else
				{
					float Angle = Transform == "rotate_cw" ? pi / 2 : -pi / 2;
					if(pTiles->IsEntitiesLayer())
					{
						for(const auto &p : pMap->m_pGameGroup->m_vpLayers)
							if(p->IsEntitiesLayer())
								p->BrushRotate(Angle);
					}
					else
						pTiles->BrushRotate(Angle);
				}
				return Done();
			}
			int X = R.Int("x", 0, 0, pTiles->m_Width - 1), Y = R.Int("y", 0, 0, pTiles->m_Height - 1);
			int Width = 1, Height = 1;
			if(Op != "tiles_set")
			{
				Width = R.Int("width", 1, 1, pTiles->m_Width - X);
				Height = R.Int("height", 1, 1, pTiles->m_Height - Y);
			}
			if(!R.m_Error.empty())
			{
				Error = R.m_Error;
				return false;
			}
			if(Op == "tiles_get")
			{
				if(!Finish())
					return false;
				W.BeginArray();
				for(int y = Y; y < Y + Height; ++y)
					for(int x = X; x < X + Width; ++x)
					{
						W.BeginObject();
						Int(W, "x", x);
						Int(W, "y", y);
						TileFields(nullptr, &W, pTiles, y * pTiles->m_Width + x);
						W.EndObject();
					}
				W.EndArray();
				return true;
			}
			// A scratch layer makes validation atomic, including specialized tile data.
			auto pScratchLayer = pTiles->Duplicate();
			auto *pScratch = static_cast<CLayerTiles *>(pScratchLayer.get());
			for(int y = Y; y < Y + Height; ++y)
				for(int x = X; x < X + Width; ++x)
					TileFields(&R, nullptr, pScratch, y * pTiles->m_Width + x);
			if(!Finish())
				return false;
			const size_t Count = (size_t)pTiles->m_Width * pTiles->m_Height;
			mem_copy(pTiles->m_pTiles, pScratch->m_pTiles, Count * sizeof(CTile));
#define COPY(type, array, tiletype) \
	if(auto *p = dynamic_cast<type *>(pTiles)) \
		mem_copy(p->array, static_cast<type *>(pScratch)->array, Count * sizeof(tiletype));
			COPY(CLayerTele, m_pTeleTile, CTeleTile)
			COPY(CLayerTune, m_pTuneTile, CTuneTile)
			COPY(CLayerSwitch, m_pSwitchTile, CSwitchTile)
			COPY(CLayerSpeedup, m_pSpeedupTile, CSpeedupTile)
#undef COPY
			return Done();
		}
		if(Op == "quad_add" || Op == "quad_set" || Op == "quad_delete")
		{
			if(!pQuads)
			{
				Error = "Expected a quad layer";
				return false;
			}
			int Id = Op == "quad_add" ? -1 : R.Int("quad", -1, 0, pQuads->m_vQuads.size() - 1);
			if(Op != "quad_add" && Id < 0)
				R.Fail("quad is required");
			CQuad Q{};
			if(Id >= 0)
				Q = pQuads->m_vQuads[Id];
			else
			{
				Q.m_PosEnv = Q.m_ColorEnv = -1;
				for(auto &C : Q.m_aColors)
					C = CColor{255, 255, 255, 255};
				Q.m_aPoints[1].x = Q.m_aPoints[3].x = 64 * 1024;
				Q.m_aPoints[2].y = Q.m_aPoints[3].y = 64 * 1024;
				Q.m_aPoints[4] = CPoint{32 * 1024, 32 * 1024};
				Q.m_aTexcoords[1].x = Q.m_aTexcoords[3].x = 1024;
				Q.m_aTexcoords[2].y = Q.m_aTexcoords[3].y = 1024;
			}
			if(Op != "quad_delete")
				QuadFields(&R, nullptr, Q, pMap->m_vpEnvelopes.size());
			if(!Finish())
				return false;
			if(Op == "quad_delete")
			{
				pQuads->m_vQuads.erase(pQuads->m_vQuads.begin() + Id);
				pMap->DeselectQuads();
				pMap->DeselectQuadPoints();
			}
			else if(Id >= 0)
				pQuads->m_vQuads[Id] = Q;
			else
			{
				pQuads->m_vQuads.push_back(Q);
				Id = pQuads->m_vQuads.size() - 1;
			}
			Modified(pMap);
			W.WriteIntValue(Id);
			return true;
		}
		if(Op == "source_add" || Op == "source_set" || Op == "source_delete")
		{
			if(!pSounds)
			{
				Error = "Expected a sound layer";
				return false;
			}
			int Id = Op == "source_add" ? -1 : R.Int("source", -1, 0, pSounds->m_vSources.size() - 1);
			if(Op != "source_add" && Id < 0)
				R.Fail("source is required");
			CSoundSource S{};
			if(Id >= 0)
				S = pSounds->m_vSources[Id];
			else
			{
				S.m_PosEnv = S.m_SoundEnv = -1;
				S.m_Shape.m_Type = CSoundShape::SHAPE_CIRCLE;
				S.m_Shape.m_Circle.m_Radius = 1500;
				S.m_Falloff = 80;
				S.m_Pan = S.m_Loop = 1;
			}
			if(Op != "source_delete")
				SourceFields(&R, nullptr, S, pMap->m_vpEnvelopes.size());
			if(!Finish())
				return false;
			if(Op == "source_delete")
			{
				pSounds->m_vSources.erase(pSounds->m_vSources.begin() + Id);
				pMap->m_SelectedSoundSource = -1;
			}
			else if(Id >= 0)
				pSounds->m_vSources[Id] = S;
			else
			{
				pSounds->m_vSources.push_back(S);
				Id = pSounds->m_vSources.size() - 1;
			}
			Modified(pMap);
			W.WriteIntValue(Id);
			return true;
		}
		Error = "Unknown operation";
		return false;
	}
} // namespace

bool RunEditorOperations(CEditor *pEditor, const json_value &Operations, std::vector<std::string> &vResults, std::string &Error)
{
	if(Operations.type != json_array || Operations.u.array.length > 10000)
	{
		Error = "operations must be an array of at most 10000 commands";
		return false;
	}
	bool Success = true;
	for(unsigned i = 0; i < Operations.u.array.length; ++i)
	{
		// Write to a separate writer so failed validation cannot leave an
		// incomplete object in the response.
		CJsonStringWriter Item;
		bool Mutation = ModifiesContent(Operations[i]);
		CEditorMap *pMap = pEditor->Map();
		CEditorAutomationState::CSnapshot Before;
		if(Mutation)
			Before = CaptureSnapshot(pMap);
		if(!Execute(pEditor, Operations[i], Item, Error))
		{
			// Native append may fail after partial edits. Restore failed
			// mutations without discarding earlier successful commands.
			if(Mutation && pEditor->Map() == pMap && !str_comp(Operations[i]["op"].u.string.ptr, "append"))
				RestoreSnapshot(pEditor, Before);
			Success = false;
			Error = "Operation " + std::to_string(i) + ": " + Error;
			break;
		}
		if(Mutation && pEditor->Map() == pMap)
		{
			if(!pMap->m_pAutomationState)
				pMap->m_pAutomationState = std::make_shared<CEditorAutomationState>();
			auto &State = *pMap->m_pAutomationState;
			State.m_Redo.clear();
			State.m_Undo.push_back(std::move(Before));
			if(State.m_Undo.size() > 8)
				State.m_Undo.pop_front();
		}
		// Retain each result as JSON text; the MCP layer embeds it in its response.
		vResults.push_back(Item.GetOutputString());
	}
	return Success;
}
