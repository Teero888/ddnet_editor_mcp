#include "mcp_tools.h"

#include <initializer_list>
#include <utility>
#include <vector>

// The single definition of every MCP tool: name, description, parameter
// schema and hints. The schemas are JSON Schema fragments assembled as text.
// Adding an operation here publishes it; its behavior lives in automation.cpp.
namespace
{
	// Property name and its schema, in declaration order.
	using CProperties = std::vector<std::pair<std::string, std::string>>;

	enum
	{
		READ_ONLY = 1 << 0,
		DESTRUCTIVE = 1 << 1,
		OPEN_WORLD = 1 << 2,
	};

	struct SOperation
	{
		const char *m_pName;
		const char *m_pDescription;
		CProperties m_Properties = {};
		std::vector<const char *> m_vRequired = {};
		int m_Flags = 0;
	};

	struct STool
	{
		const char *m_pName;
		const char *m_pDescription;
		std::string m_Schema;
		bool m_ReadOnly = false;
	};

	std::string Quote(const std::string &Text)
	{
		std::string Result = "\"";
		for(const char Character : Text)
		{
			if(Character == '"' || Character == '\\')
				Result += '\\';
			Result += Character;
		}
		return Result + "\"";
	}

	const char *Bool(bool Value)
	{
		return Value ? "true" : "false";
	}

	std::string Integer(long long Min)
	{
		return "{\"type\":\"integer\",\"minimum\":" + std::to_string(Min) + "}";
	}

	std::string Integer(long long Min, long long Max)
	{
		return "{\"type\":\"integer\",\"minimum\":" + std::to_string(Min) + ",\"maximum\":" + std::to_string(Max) + "}";
	}

	std::string String(int MaxLength = -1)
	{
		return std::string("{\"type\":\"string\"") + (MaxLength >= 0 ? ",\"maxLength\":" + std::to_string(MaxLength) : "") + "}";
	}

	std::string Enum(std::initializer_list<const char *> Values)
	{
		std::string Result = "{\"type\":\"string\",\"enum\":[";
		for(const char *pValue : Values)
			Result += (Result.back() == '[' ? "" : ",") + Quote(pValue);
		return Result + "]}";
	}

	std::string Array(const std::string &Items, int MinItems = -1, int MaxItems = -1, const char *pDescription = nullptr)
	{
		std::string Result = "{\"type\":\"array\",\"items\":" + Items;
		if(MinItems >= 0)
			Result += ",\"minItems\":" + std::to_string(MinItems);
		if(MaxItems >= 0)
			Result += ",\"maxItems\":" + std::to_string(MaxItems);
		if(pDescription)
			Result += ",\"description\":" + Quote(pDescription);
		return Result + "}";
	}

	// Objects reject fields that are not listed.
	std::string Object(const CProperties &Properties = {}, const std::vector<const char *> &vRequired = {})
	{
		std::string Result = "{\"type\":\"object\",\"properties\":{";
		for(const auto &[Name, Schema] : Properties)
			Result += (Result.back() == '{' ? "" : ",") + Quote(Name) + ":" + Schema;
		Result += "},\"required\":[";
		for(const char *pName : vRequired)
			Result += (Result.back() == '[' ? "" : ",") + Quote(pName);
		return Result + "],\"additionalProperties\":false}";
	}

	// Concatenates property lists. A repeated name keeps its first position and takes the last schema.
	CProperties Merge(std::initializer_list<CProperties> Lists)
	{
		CProperties Result;
		for(const auto &List : Lists)
		{
			for(const auto &Property : List)
			{
				bool Found = false;
				for(auto &Existing : Result)
				{
					if(Existing.first == Property.first)
					{
						Existing.second = Property.second;
						Found = true;
					}
				}
				if(!Found)
					Result.push_back(Property);
			}
		}
		return Result;
	}

	// Properties named <prefix><index> for every prefix, e.g. x0..x4 then y0..y4.
	CProperties Indexed(std::initializer_list<const char *> Prefixes, int Count, const std::string &Schema)
	{
		CProperties Result;
		for(const char *pPrefix : Prefixes)
			for(int i = 0; i < Count; ++i)
				Result.emplace_back(pPrefix + std::to_string(i), Schema);
		return Result;
	}

	std::vector<SOperation> Operations()
	{
		const std::string STRING = String();
		const std::string INDEX = Integer(0);
		const std::string REFERENCE = Integer(-1);
		const std::string TOGGLE = Integer(0, 1);
		const std::string BYTE = Integer(0, 255);
		const std::string COORD = Integer(-2147483648LL, 2147483647);
		const std::string WORLD = Integer(-10000000, 10000000);
		const std::string LAYER_SIZE = Integer(1, 4194304);
		const std::string TRANSFORM = Enum({"flip_x", "flip_y", "rotate_cw", "rotate_ccw"});
		const CProperties POSITION = {{"group", INDEX}, {"layer", INDEX}};
		const CProperties TILE = {{"x", INDEX}, {"y", INDEX}, {"index", BYTE}, {"flags", Integer(0, 15)}, {"number", BYTE}, {"delay", BYTE}, {"force", BYTE}, {"max_speed", BYTE}, {"angle", Integer(-359, 359)}};
		const CProperties RECT = {{"x", INDEX}, {"y", INDEX}, {"width", Integer(1)}, {"height", Integer(1)}};
		const CProperties GROUP = {{"name", STRING}, {"offset_x", WORLD}, {"offset_y", WORLD}, {"parallax_x", Integer(-100000, 100000)}, {"parallax_y", Integer(-100000, 100000)}, {"clipping", TOGGLE}, {"clip_x", WORLD}, {"clip_y", WORLD}, {"clip_w", Integer(0, 10000000)}, {"clip_h", Integer(0, 10000000)}, {"visible", TOGGLE}};
		const CProperties LAYER = {{"name", STRING}, {"flags", Integer(0, 1)}, {"visible", TOGGLE}, {"readonly", TOGGLE}, {"image", REFERENCE}, {"sound", REFERENCE}, {"r", BYTE}, {"g", BYTE}, {"b", BYTE}, {"a", BYTE}, {"color_env", REFERENCE}, {"color_env_offset", COORD}, {"automapper_config", REFERENCE}, {"automapper_reference", Integer(-1, 9)}, {"seed", Integer(0)}, {"auto_automapper", TOGGLE}, {"overlays", TOGGLE}};
		const CProperties QUAD = Merge({Indexed({"x", "y"}, 5, COORD), Indexed({"u", "v"}, 4, COORD), Indexed({"r", "g", "b", "a"}, 4, BYTE), {{"pos_env", REFERENCE}, {"color_env", REFERENCE}, {"pos_env_offset", COORD}, {"color_env_offset", COORD}}});
		const CProperties SOURCE = {{"x", COORD}, {"y", COORD}, {"loop", TOGGLE}, {"pan", TOGGLE}, {"delay", Integer(0)}, {"falloff", BYTE}, {"pos_env", REFERENCE}, {"sound_env", REFERENCE}, {"pos_env_offset", COORD}, {"sound_env_offset", COORD}, {"shape", Integer(0, 1)}, {"radius", Integer(0)}, {"width", Integer(0)}, {"height", Integer(0)}};
		const CProperties POINT = Merge({{{"time", Integer(0)}, {"curve", Integer(0, 5)}, {"values", Array(COORD, 1, 4)}}, Indexed({"in_dx", "in_dy", "out_dx", "out_dy"}, 4, COORD)});
		const CProperties ENVELOPE = {{"name", STRING}, {"synchronized", TOGGLE}, {"points", Array(Object(POINT, {"values"}))}};

		return {
			{"inspect", "Inspect the open map, groups, layers, resources, envelopes, quads and sound sources.", {}, {}, READ_ONLY},
			{"new", "Open a new default map in another editor tab."},
			{"tabs", "List open map tabs.", {}, {}, READ_ONLY},
			{"actions", "List all native editor quick actions, names and availability.", {}, {}, READ_ONLY},
			{"status", "Get saving, dialog and modified state.", {}, {}, READ_ONLY},
			{"constants", "Get native tile IDs, entity IDs, tile flags, layer kinds and curve constants. Entity tiles use ENTITY_OFFSET + entity ID.", {}, {}, READ_ONLY},
			{"console_commands", "Discover native DDNet client console commands and parameters.", {}, {}, READ_ONLY},
			{"dialog_close", "Dismiss editor file dialogs and popup menus."},
			{"undo", "Undo the latest direct MCP map edit, restoring the preceding map content. Eight steps are retained per tab.", {}, {}, DESTRUCTIVE},
			{"redo", "Redo the latest undone direct MCP map edit.", {}, {}, DESTRUCTIVE},
			{"brush_clear", "Clear the active native editor brush."},
			{"brush_info", "Inspect active native brush layer kinds and sizes."},
			{"tab_select", "Switch to map tab (tab).", {{"tab", INDEX}}},
			{"tab_close", "Close map tab, discarding unsaved changes (tab).", {{"tab", INDEX}}, {}, DESTRUCTIVE},
			{"action", "Invoke native quick action (name from actions); may open a dialog. Includes native undo/redo and UI toggles.", {{"name", STRING}}, {"name"}, OPEN_WORLD},
			{"console", "Execute a native DDNet client console command (command). Can change settings, connect, quit or disable the editor bridge; use deliberately.", {{"command", STRING}}, {"command"}, DESTRUCTIVE | OPEN_WORLD},
			{"checkpoint", "Capture current map content in memory (name, default: default); at most 8 per map tab.", {{"name", STRING}}, {}, READ_ONLY},
			{"restore", "Restore a named in-memory checkpoint (name); does not change the map filename.", {{"name", STRING}}, {}, DESTRUCTIVE},
			{"checkpoint_delete", "Release a named checkpoint (name).", {{"name", STRING}}},
			{"load", "Open a .map file (path).", {{"path", STRING}}, {"path"}},
			{"save", "Save a map (path relative to DDNet save storage), waiting for completion and reporting disk errors. In a command batch it schedules an asynchronous save.", {{"path", STRING}}, {"path"}},
			{"save_status", "Get asynchronous save completion/error for a previously scheduled path.", {{"path", STRING}}, {"path"}, READ_ONLY},
			{"append", "Append another .map file to the current map (path).", {{"path", STRING}}, {"path"}},
			{"screenshot", "Schedule a screenshot (path); capture finishes asynchronously.", {{"path", STRING}}, {"path"}},
			{"view", "Set editor camera position in world units (x, y) and zoom percent (zoom).", {{"x", WORLD}, {"y", WORLD}, {"zoom", Integer(10, 2000)}}},
			{"view_fit", "Frame game-map bounds or a rectangle in world units (x, y, width, height, padding percent). Changes camera only.", {{"x", WORLD}, {"y", WORLD}, {"width", Integer(1, 10000000)}, {"height", Integer(1, 10000000)}, {"padding", Integer(0, 100)}}},
			{"ui", "Set mode (0 layers, 1 images, 2 sounds), gui, detail, tile_info (0 off/1 decimal/2 hex), grid, grid_factor, envelope_preview, extra (-1 none/0 envelopes/1 settings/2 history).", {{"mode", Integer(0, 2)}, {"gui", TOGGLE}, {"detail", TOGGLE}, {"tile_info", Integer(0, 2)}, {"grid", TOGGLE}, {"grid_factor", Integer(1, 15)}, {"envelope_preview", TOGGLE}, {"extra", Integer(-1, 2)}}},
			{"metadata", "Set author, version, credits and license strings.", {{"author", STRING}, {"version", STRING}, {"credits", STRING}, {"license", STRING}}},
			{"settings", "Replace map server settings (commands: array of strings).", {{"commands", Array(String(255))}}, {"commands"}},
			{"group_add", "Create a group (name); returns its index.", {{"name", STRING}}},
			{"group_set", "Set group name, offset_x/y, parallax_x/y, clipping, clip_x/y/w/h, visible.", Merge({{{"group", INDEX}}, GROUP}), {"group"}},
			{"group_delete", "Delete a non-game group (group).", {{"group", INDEX}}, {"group"}, DESTRUCTIVE},
			{"group_duplicate", "Duplicate a decorative group and its layers (group).", {{"group", INDEX}}, {"group"}},
			{"group_move", "Reorder a group (group, to).", {{"group", INDEX}, {"to", INDEX}}, {"group"}},
			{"layer_add", "Create tiles, quads, sounds, front, tele, speedup, switch or tune layer (group, kind, name, width, height); returns index.", {{"group", INDEX}, {"kind", Enum({"tiles", "quads", "sounds", "front", "tele", "speedup", "switch", "tune"})}, {"name", STRING}, {"width", LAYER_SIZE}, {"height", LAYER_SIZE}}, {"group"}},
			{"layer_set", "Set layer properties (group, layer, name, flags, visible, readonly, image/sound, r/g/b/a, color_env, color_env_offset).", Merge({POSITION, LAYER}), {"group", "layer"}},
			{"layer_delete", "Delete a non-game layer (group, layer).", POSITION, {"group", "layer"}, DESTRUCTIVE},
			{"layer_duplicate", "Duplicate a decorative layer (group, layer).", POSITION, {"group", "layer"}},
			{"select", "Select a layer in the editor (group, layer).", POSITION, {"group", "layer"}},
			{"layer_move", "Move a layer within or between groups (group, layer, to, to_group). Entity layers stay in the game group.", Merge({POSITION, {{"to", INDEX}, {"to_group", INDEX}}}), {"group", "layer"}},
			{"layer_resize", "Resize tile layer (group, layer, width, height); resizing game resizes entity layers together.", Merge({POSITION, {{"width", LAYER_SIZE}, {"height", LAYER_SIZE}}}), {"group", "layer"}},
			{"layer_shift", "Translate tile contents by dx/dy tiles, clearing exposed cells (group, layer).", Merge({POSITION, {{"dx", COORD}, {"dy", COORD}}}), {"group", "layer"}},
			{"layer_transform", "Transform tiles (group, layer, transform: flip_x, flip_y, rotate_cw, rotate_ccw).", Merge({POSITION, {{"transform", TRANSFORM}}}), {"group", "layer", "transform"}},
			{"selection", "Select layers, quads, quad_points, source and envelope (group, layer plus index arrays).", Merge({POSITION, {{"layers", Array(INDEX, 1, -1, "First selected layer must equal layer; object selections refer to it.")}, {"quads", Array(INDEX)}, {"quad_points", Array(Integer(0, 4))}, {"source", REFERENCE}, {"envelope", REFERENCE}}}), {"group", "layer"}},
			{"tiles_get", "Read rectangle of tiles (group, layer, x, y, width, height).", Merge({POSITION, RECT}), {"group", "layer"}, READ_ONLY},
			{"tiles_set", "Set one tile (group, layer, x, y, index, flags; entity fields number, delay, force, max_speed, angle).", Merge({POSITION, TILE}), {"group", "layer"}},
			{"tiles_fill", "Fill rectangle (same as tiles_set plus width, height); index 0 erases tiles.", Merge({POSITION, TILE, RECT}), {"group", "layer"}},
			{"tiles_patch", "Atomically write sparse tiles (group, layer, tiles: [{x, y, index, flags, entity fields}]); unspecified tile fields are zero.", Merge({POSITION, {{"tiles", Array(Object(TILE, {"x", "y"}), -1, 100000)}}}), {"group", "layer", "tiles"}},
			{"tiles_copy", "Copy a rectangle including entity fields (group, layer, x, y, width, height, source_group/layer/x/y). Overlaps are supported; kinds must match.", Merge({POSITION, RECT, {{"source_group", INDEX}, {"source_layer", INDEX}, {"source_x", INDEX}, {"source_y", INDEX}}}), {"group", "layer"}},
			{"automap", "Run native automapper on decorative tile layer (group, layer, config, seed, reference).", Merge({POSITION, {{"config", INDEX}, {"seed", Integer(1)}, {"reference", Integer(-1, 9)}}}), {"group", "layer"}},
			{"image_add", "Import PNG (path, optional name/external); returns image index. Images are sorted; inspect indices after import.", {{"path", STRING}, {"name", STRING}, {"external", TOGGLE}}, {"path"}},
			{"image_replace", "Replace PNG asset while preserving layer references (image, path, optional name/external). Returns its index after sorting.", {{"path", STRING}, {"name", STRING}, {"external", TOGGLE}, {"image", INDEX}}, {"path", "image"}},
			{"image_set", "Set image name and embedding (image, name, external: 0 or 1); inspect indices after renaming.", {{"image", INDEX}, {"external", TOGGLE}, {"name", STRING}}, {"image"}},
			{"image_delete", "Delete image and repair layer references (image).", {{"image", INDEX}}, {"image"}, DESTRUCTIVE},
			{"image_export", "Export an image asset as PNG (image, path). Returns resolved file path.", {{"image", INDEX}, {"path", STRING}}, {"image", "path"}},
			{"sound_add", "Import Opus audio (path); returns sound index.", {{"path", STRING}, {"name", STRING}}, {"path"}},
			{"sound_replace", "Replace Opus asset while preserving sound-layer references (sound, path, optional name).", {{"path", STRING}, {"name", STRING}, {"sound", INDEX}}, {"path", "sound"}},
			{"sound_delete", "Delete sound and repair layer references (sound).", {{"sound", INDEX}}, {"sound"}, DESTRUCTIVE},
			{"sound_stop", "Stop playback of a sound asset (sound).", {{"sound", INDEX}}, {"sound"}},
			{"sound_export", "Export original Opus audio (sound, path). Returns resolved file path.", {{"sound", INDEX}, {"path", STRING}}, {"sound", "path"}},
			{"sound_play", "Preview sound through DDNet's audio device (sound, loop: 0/1).", {{"sound", INDEX}, {"loop", TOGGLE}}, {"sound"}},
			{"quad_add", "Create quad (group, layer, x0..x4, y0..y4, u0..u3, v0..v3, r0..r3/g/b/a, pos_env, color_env and offsets); returns index. Coordinates use 1024 units per world unit.", Merge({POSITION, QUAD}), {"group", "layer"}},
			{"quad_set", "Edit quad (same fields as quad_add plus quad index).", Merge({POSITION, QUAD, {{"quad", INDEX}}}), {"group", "layer", "quad"}},
			{"quad_delete", "Delete quad (group, layer, quad).", Merge({POSITION, {{"quad", INDEX}}}), {"group", "layer", "quad"}, DESTRUCTIVE},
			{"quad_duplicate", "Duplicate quad (group, layer, quad); returns the new index.", Merge({POSITION, {{"quad", INDEX}}}), {"group", "layer", "quad"}},
			{"quad_move", "Reorder quad (group, layer, quad, to).", Merge({POSITION, {{"quad", INDEX}, {"to", INDEX}}}), {"group", "layer", "quad"}},
			{"source_add", "Create sound source (group, layer, x/y in 1024 fixed point, loop, pan, delay, falloff, pos_env, sound_env and offsets, shape: 0 rectangle/1 circle, width/height in fixed point or radius in world units); returns index.", Merge({POSITION, SOURCE}), {"group", "layer"}},
			{"source_set", "Edit sound source (same fields as source_add plus source index).", Merge({POSITION, SOURCE, {{"source", INDEX}}}), {"group", "layer", "source"}},
			{"source_delete", "Delete sound source (group, layer, source).", Merge({POSITION, {{"source", INDEX}}}), {"group", "layer", "source"}, DESTRUCTIVE},
			{"source_duplicate", "Duplicate sound source (group, layer, source); returns new index.", Merge({POSITION, {{"source", INDEX}}}), {"group", "layer", "source"}},
			{"source_move", "Reorder sound source (group, layer, source, to).", Merge({POSITION, {{"source", INDEX}, {"to", INDEX}}}), {"group", "layer", "source"}},
			{"envelope_add", "Create envelope (name, channels: 1 sound/3 position/4 color, synchronized, points: [{time milliseconds, curve: 0..5, values: fixed point integers, optional in_dx0..3/in_dy/out_dx/out_dy}]); returns index.", Merge({ENVELOPE, {{"channels", "{\"type\":\"integer\",\"enum\":[1,3,4]}"}}})},
			{"envelope_set", "Edit envelope (envelope, name, synchronized, points).", Merge({ENVELOPE, {{"envelope", INDEX}}}), {"envelope"}},
			{"envelope_delete", "Delete envelope and repair references (envelope).", {{"envelope", INDEX}}, {"envelope"}, DESTRUCTIVE},
			{"envelope_duplicate", "Duplicate an envelope (envelope); returns new index.", {{"envelope", INDEX}}, {"envelope"}},
			{"envelope_move", "Reorder envelope and repair its references (envelope, to).", {{"envelope", INDEX}, {"to", INDEX}}, {"envelope"}},
			{"tile_art", "Convert a PNG into native colored tile-art layers and images (path). Returns group index.", {{"path", STRING}}, {"path"}},
			{"quad_art", "Convert a PNG into native quad art (path, image_pixel_size, quad_pixel_size in world units, centralize, optimize). Returns group index.", {{"path", STRING}, {"image_pixel_size", Integer(1, 1024)}, {"quad_pixel_size", Integer(1, 100000)}, {"centralize", TOGGLE}, {"optimize", TOGGLE}}, {"path"}},
			{"tiles_text", "Write ASCII letters, digits, spaces and newlines into a decorative DDNet font-atlas layer (group, layer, x/y in tiles, text). Letters use 1..26 and digits use 54..63.", Merge({POSITION, {{"text", STRING}, {"x", INDEX}, {"y", INDEX}}}), {"group", "layer", "text"}},
			{"brush_grab", "Grab a native brush from a layer (group, layer, x/y/width/height in WORLD units; tile cells are 32 world units). Replaces the active brush.", Merge({POSITION, {{"x", COORD}, {"y", COORD}, {"width", Integer(1)}, {"height", Integer(1)}}}), {"group", "layer"}},
			{"brush_stamp", "Stamp the active brush onto a compatible layer (group, layer, x/y in WORLD units, brush_layer, destructive). Supports tiles, quads and sounds.", Merge({POSITION, {{"x", COORD}, {"y", COORD}, {"brush_layer", INDEX}, {"destructive", TOGGLE}}}), {"group", "layer"}},
			{"brush_transform", "Flip or rotate the active native brush (transform).", {{"transform", TRANSFORM}}, {"transform"}},
		};
	}

	// Batch items are the operation schemas with a discriminating "op" field.
	std::string CommandSchema(const std::vector<SOperation> &vOperations)
	{
		std::string Result = "{\"oneOf\":[";
		for(const auto &Operation : vOperations)
		{
			std::vector<const char *> vRequired = {"op"};
			vRequired.insert(vRequired.end(), Operation.m_vRequired.begin(), Operation.m_vRequired.end());
			const std::string Name = "{\"type\":\"string\",\"const\":" + Quote(Operation.m_pName) + "}";
			Result += (Result.back() == '[' ? "" : ",") + Object(Merge({{{"op", Name}}, Operation.m_Properties}), vRequired);
		}
		return Result + "]}";
	}

	// Tools that are not a single editor operation; mcp.cpp implements them.
	std::vector<STool> HelperTools(const std::vector<SOperation> &vOperations)
	{
		const std::string PREVIEW = Object({{"max_size", Integer(64, 4096)}, {"include_ui", "{\"type\":\"boolean\"}"}});
		return {
			{"editor_operations", "Discover all operations, full parameter schemas, coordinate conventions and capabilities. Does not contact the editor.", Object(), true},
			{"editor_commands", "Execute commands sequentially against the editor's current map. Stops on first error; earlier commands remain applied. Direct map edits support editor_undo/editor_redo. Connecting does not start DDNet or edit anything.", Object({{"operations", Array(CommandSchema(vOperations), -1, 10000)}}, {"operations"})},
			{"editor_viewport", "Look at the current editor viewport. Returns an actual MCP PNG image plus camera/crop metadata. Default crops out editor panels; include_ui=true shows the full window. Does not change the camera or map. DDNet must be open; its window does not need focus.", PREVIEW, true},
			{"editor_overview", "Frame the game map bounds and return a PNG of the editor viewport. Changes camera only. For very large maps inspect regions with editor_view then editor_viewport.", PREVIEW},
			{"editor_asset_image", "See a map image/tileset as an MCP PNG image, including embedded assets. Returns dimensions and an image index for exact tile work.", Object({{"image", Integer(0)}, {"max_size", Integer(64, 4096)}}, {"image"}), true},
			{"editor_asset_audio", "Return a map sound as MCP audio content (audio/ogg, Opus). Playback/analysis availability depends on your MCP host's audio support. Use editor_sound_export for file output.", Object({{"sound", Integer(0)}}, {"sound"}), true},
			{"editor_import_asset", "Upload base64 PNG or Ogg Opus data directly into the editor, optionally replacing an existing image/sound index. Returns resulting index; inspect image indices after import.", Object({{"kind", Enum({"image", "sound"})}, {"data_base64", String(25165824)}, {"name", String()}, {"replace", Integer(0)}}, {"kind", "data_base64", "name"})},
			{"editor_create_tileset", "Create a PNG tile atlas from explicit RGBA colors or pixel grids. Tile IDs are 1..255; 0 is transparent air. Then import with editor_image_add. Existing files are not overwritten.", Object({{"path", String()}, {"tiles", "{\"type\":\"object\"}"}, {"tile_size", Integer(1, 128)}}, {"path", "tiles"})},
		};
	}

	std::string BuildToolCatalog()
	{
		const std::vector<SOperation> vOperations = Operations();
		std::string Result = "{\"tools\":[";
		for(const auto &Operation : vOperations)
		{
			Result += std::string(Result.back() == '[' ? "" : ",") + "{\"name\":" + Quote(std::string("editor_") + Operation.m_pName) + ",\"description\":" + Quote(Operation.m_pDescription) + ",\"inputSchema\":" + Object(Operation.m_Properties, Operation.m_vRequired) +
				  ",\"annotations\":{\"readOnlyHint\":" + Bool(Operation.m_Flags & READ_ONLY) + ",\"destructiveHint\":" + Bool(Operation.m_Flags & DESTRUCTIVE) + ",\"openWorldHint\":" + Bool(Operation.m_Flags & OPEN_WORLD) + "}}";
		}
		for(const auto &Tool : HelperTools(vOperations))
		{
			Result += std::string(",{\"name\":") + Quote(Tool.m_pName) + ",\"description\":" + Quote(Tool.m_pDescription) + ",\"inputSchema\":" + Tool.m_Schema +
				  ",\"annotations\":{\"readOnlyHint\":" + Bool(Tool.m_ReadOnly) + ",\"openWorldHint\":false}}";
		}
		return Result + "]}";
	}

	std::string BuildOperationCatalog()
	{
		std::string Result = "{\"operations\":{";
		for(const auto &Operation : Operations())
			Result += (Result.back() == '{' ? "" : ",") + Quote(Operation.m_pName) + ":{\"description\":" + Quote(Operation.m_pDescription) + ",\"parameters\":" + Object(Operation.m_Properties, Operation.m_vRequired) + "}";
		const CProperties Conventions = {
			{"indices", "zero-based; image indices may change after imports or renaming"},
			{"tile_coordinates", "tiles"},
			{"quad_and_source_positions", "1024 fixed-point units per world unit"},
			{"envelope_time", "milliseconds"},
			{"envelope_values", "1024 fixed point"},
			{"toggles", "integer 0 or 1"},
			{"batch", "sequential; earlier successful commands remain applied on error"},
			{"history", "direct map edits retain eight undo snapshots per tab; native actions use native history"},
			{"connection", "Start native MCP from the editor MCP panel and connect using its HTTP URL. Connecting never edits the map"},
		};
		Result += "},\"conventions\":{";
		for(const auto &[Name, Text] : Conventions)
			Result += (Result.back() == '{' ? "" : ",") + Quote(Name) + ":" + Quote(Text);
		return Result + "}}";
	}
}

namespace EditorMcp
{
	const std::string &ToolCatalog()
	{
		static const std::string s_Catalog = BuildToolCatalog();
		return s_Catalog;
	}

	const std::string &OperationCatalog()
	{
		static const std::string s_Catalog = BuildOperationCatalog();
		return s_Catalog;
	}
}
