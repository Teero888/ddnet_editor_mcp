# DDNet Editor MCP

DDNet hosts a native MCP server inside the map editor. It exposes the same
90 discoverable tools over a local HTTP endpoint. Starting the server and
connecting an agent do not open or modify maps. The server, its tool catalog
and its parameter validation are implemented in C++ only; there is no script
or code generation step.

## Connect from the editor UI

1. Open the map editor in the DDNet build from this checkout.
2. Click **MCP** in the editor's top menu bar.
3. Choose a port (default **8305**) and click **Start MCP server**.
4. Click **Copy URL**, then add an HTTP MCP connection in your agent's MCP
   settings and paste the URL:

   ```text
   http://127.0.0.1:8305/mcp
   ```

The panel also has **Copy MCP configuration** for clients that accept
`mcpServers` JSON. The example in
[examples/editor_mcp.json](examples/editor_mcp.json) uses the native endpoint:

```json
{
  "mcpServers": {
    "ddnet-editor": {
      "url": "http://127.0.0.1:8305/mcp"
    }
  }
}
```

The agent's host must support **Streamable HTTP** MCP and run on the same
computer, or forward this local endpoint. Adding the URL to the host's MCP
settings makes tools available to the agent; a chat message containing the URL
alone does not register tools in a host that requires connection setup.

Keep the editor open. **Stop MCP server** disconnects clients, and leaving the
editor stops the listener. The port is remembered; the server starts only when
you press its Start button. The request counter shows traffic from MCP clients.
A failed port bind appears in the panel so you can choose a different port.

The listener binds to IPv4 loopback only. It validates HTTP Host/Origin headers.
POST requests return ordinary MCP JSON responses; GET returns 405 because this
stateless server does not offer an SSE notification stream. Editor tools run on
the editor thread and are serialized across connected clients, including
asynchronous saves and screenshots. HTTP requests have a 30-second deadline,
32 MiB body limit and a maximum of 16 concurrent connections. A request that
expires before response transmission receives HTTP 408 with a JSON-RPC error
and its request ID, when available. A timeout or connection loss does not undo
an operation that has already executed; inspect editor state before retrying.

Build DDNet from this checkout to include the native server. The development
build currently lives at `/tmp/ddnet-editor-build/DDNet`; launch that executable
instead of a previously installed DDNet version.

## MCP tools

The server exposes **90 tools** with discoverable JSON parameter schemas:
82 native operations, each available as `editor_<operation>`, plus eight helper
tools. `editor_operations` returns descriptions, schemas and coordinate
conventions. `editor_commands` executes a sequential batch of native operations.

The model can see the map and its assets through actual MCP media responses:

- `editor_viewport` returns a PNG of the current map viewport. `include_ui: true`
  includes the editor interface. It keeps the current camera position.
- `editor_overview` fits the camera to game-layer bounds and returns a PNG. It
  changes the camera only. Use `editor_view_fit` with a world rectangle to frame
  a particular area, then `editor_viewport` for a closer look.
- `editor_asset_image` returns a PNG of an image or tileset, including embedded
  assets. `editor_asset_audio` returns the original Ogg Opus as MCP audio.
  Audio playback or analysis depends on the MCP host's support.

Image previews accept `max_size` (64..4096 pixels, default 1200), return source
and output dimensions, and clean up their temporary exports. Screenshot
capture does not need input focus: the DDNet window can be in the background or
on another workspace, as long as it is not closed and `gfx_backgroundrender`
is enabled (the default). Native previews time out after 10 seconds if the PNG
is missing or incomplete, instead of waiting for the HTTP connection to close.
`editor_status` reports `window_active` and `window_open`; only `window_open`
matters for capture. Native previews use DDNet's PNG loader and image
resampler. Export other formats directly to a file if needed.

`editor_import_asset` uploads base64 PNG or Ogg Opus data and can replace an
existing resource. Uploads are limited to 16 MiB; larger assets can be imported
by path with `editor_image_add` or `editor_sound_add`.
`editor_create_tileset` generates a PNG atlas from explicit RGBA colors or pixel
grids. Import its output separately. Existing atlas files are not overwritten.

For example, call `editor_commands` with:

```json
{
  "operations": [
    {"op": "checkpoint", "name": "before-edit"},
    {"op": "tiles_fill", "group": 1, "layer": 0,
     "x": 5, "y": 10, "width": 12, "height": 1, "index": 1},
    {"op": "inspect"}
  ]
}
```

Inspect the map before choosing indices: groups, layers, resources, quads,
sources, envelopes, and tabs all use zero-based indices. Adding images sorts
images and repairs existing references, so inspect indices again after imports.
Creating or duplicating groups, layers, images, sounds, quads, sources, and envelopes returns the
created object's current index. Other write operations generally return null.

## Supported operations

| Area | Native operations (prefix each with `editor_` for its MCP tool) |
| --- | --- |
| Map files and tabs | `new`, `load`, `save`, `save_status`, `append`, `tabs`, `tab_select`, `tab_close` |
| Inspection and UI | `inspect`, `status`, `constants`, `select`, `selection`, `view`, `view_fit`, `ui`, `screenshot`, `dialog_close`, `actions`, `action`, `console_commands`, `console` |
| Groups | `group_add`, `group_set`, `group_delete`, `group_move`, `group_duplicate` |
| Layers | `layer_add`, `layer_set`, `layer_delete`, `layer_duplicate`, `layer_move`, `layer_resize`, `layer_transform`, `layer_shift` |
| Tiles and art | `tiles_get`, `tiles_set`, `tiles_fill`, `tiles_patch`, `tiles_copy`, `tiles_text`, `automap`, `tile_art`, `quad_art` |
| Brushes | `brush_grab`, `brush_stamp`, `brush_transform`, `brush_info`, `brush_clear` |
| Images | `image_add`, `image_replace`, `image_set`, `image_delete`, `image_export` |
| Audio | `sound_add`, `sound_replace`, `sound_delete`, `sound_export`, `sound_play`, `sound_stop` |
| Quads | `quad_add`, `quad_set`, `quad_delete`, `quad_duplicate`, `quad_move` |
| Sound sources | `source_add`, `source_set`, `source_delete`, `source_duplicate`, `source_move` |
| Animation envelopes | `envelope_add`, `envelope_set`, `envelope_delete`, `envelope_duplicate`, `envelope_move` |
| Metadata and settings | `metadata`, `settings` |
| Recovery | `undo`, `redo`, `checkpoint`, `restore`, `checkpoint_delete` |

`actions` lists the editor's native quick actions and whether they are enabled.
`action` invokes one by name, including UI toggles and native history actions.
Some actions open native dialogs and require interaction in DDNet. Use the
explicit file operations for scripted imports and saves. `console_commands`
exposes the native client console API and `console` executes its commands.
These include connection, configuration, quit and bridge-disable commands;
operations that close the editor can prevent later requests from being processed.
This API does not automate arbitrary modal dialog fields or synthetic mouse input;
use the direct tools for map data edits.

Layers can be decorative tiles, quads, sounds, front, teleport, speedup, switch,
or tune layers. A new map already has its game layer. The game group and layer
cannot be deleted. Entity layers are unique, belong to the game group, and have
matching dimensions. Resize the game layer to resize all entity layers. Rotating
an entity layer rotates all entity layers to maintain dimensions. Decorative
layers can be duplicated and reordered. Decorative layers can move between groups.

Tile coordinates and rectangle dimensions are measured in tiles. Tile `index`
is 0..255; index 0 erases. Tile writes replace a tile's index, flags and entity
fields. Unspecified entity fields default to zero. Entity fields include
`number`, `delay`, `force`, `max_speed`, and `angle`, depending on the layer.
Native map saving may add the opaque bit (4) to decorative tile flags.

Quad and sound-source position coordinates use native fixed point: multiply
world units by 1024. Quad texture coordinates range from 0 to 1024 for a complete
texture. `x0..x3`/`y0..y3` are the corners and `x4`/`y4` the pivot. Color channels
`r0..r3`, `g0..g3`, `b0..b3`, `a0..a3` are 0..255. Sound rectangle dimensions use
fixed point; sound circle radius uses world units. Envelope point times and
Bezier tangent horizontal deltas use milliseconds, and values and vertical
deltas use native 1024 fixed point. Envelope channel counts are 1 (sound), 3
(position), and 4 (color). Curve values are 0..5. Integer toggles use 0 or 1.

Imports accept DDNet storage paths or absolute paths. Native atlas generation
uses save-storage or absolute paths; its parent directory must already exist. Image imports accept PNG;
sound imports accept Opus. Native map saves use paths relative to DDNet's save
storage, such as `maps/my-map.map`. `editor_save` waits for the native save job
and reports asynchronous disk errors, returning the resolved path and file size.
A `save` inside `editor_commands` schedules the job; call
`save_status` with the same path to check `finished`, `saving` and `error`.
Completion results track up to 32 requested save paths; tracking can expire
when additional save paths are requested. Screenshots
are asynchronous; the visual helper tools wait for complete PNG output.

## Checkpoints and execution behavior

Direct map edits have a separate automation history: `editor_undo` and
`editor_redo` retain up to eight snapshots per tab. They restore map geometry,
metadata, settings, envelopes and resource references. Direct mutations clear
native undo histories because their stored indices and pointers can become
invalid. Native quick actions continue to use the editor's native history.
An automation undo restores a whole content snapshot, so manual edits made
after that snapshot are also reverted.

Use a named `checkpoint` before a larger sequence and `restore` to recover it.
Checkpoints are scoped to the current map tab and disappear when the tab closes.
There are at most eight per tab. Histories and checkpoints retain image and sound
objects; manually replacing an asset through the native UI can change a retained
asset. MCP asset replacements create separate resource objects and are undoable.
Checkpoints do not restore the filename, overwrite files on disk, or restore
native undo histories. Snapshots of large maps retain substantial memory.

Commands validate parameters before applying a mutation and reject unknown
fields. A batch runs sequentially and stops at the first error. **Earlier
commands remain applied.** Error responses include the completed results.
Commands affect the current tab at the time each command executes.

## Tool definitions

`src/game/editor/mcp_tools.cpp` is the single definition of every tool: its
name, description, parameter schema and read-only/destructive hints. The
`tools/list` response, the `editor_operations` result, the `editor_commands`
batch schema and request validation are all built from that table at startup.
To add an operation, add its entry there and implement it in
`src/game/editor/automation.cpp`.

`editor_create_tileset` takes a `tiles` object such as
`{"1": [255, 0, 0, 255], "2": [0, 255, 0, 255]}`. The atlas has 16 columns and
16 rows. Tile 0 stays transparent. Each supplied tile can be a solid RGBA color
or a square grid of RGBA pixels. Pixel grids must match `tile_size`. Assign the
imported image to a decorative tile layer with `layer_set`, then place its tile
indices with `tiles_set` or `tiles_fill`.

## Tests

Native transport and protocol tests can run without starting a DDNet client:

```sh
cmake --build /tmp/ddnet-editor-build --target editor-mcp-tests
/tmp/ddnet-editor-build/editor-mcp-tests --gtest_filter='CTransportTest.*:EditorMcpProtocol.*'
```

These exercise loopback HTTP, fragmented requests, origin/host validation,
listener lifecycle, deferred responses, inert discovery, tool catalog consistency,
JSON-RPC IDs and parameter validation. They require the development GTest
library. They do not exercise a live editor's rendering or map mutations.

For example, a visual editing sequence is `editor_inspect`,
`editor_constants`, `editor_overview`, a named checkpoint, editing tools,
`editor_viewport`, then `editor_save` when the user requests saving. Connecting
alone never invokes this sequence.
