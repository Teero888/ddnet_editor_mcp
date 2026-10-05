#ifndef GAME_EDITOR_AUTOMATION_H
#define GAME_EDITOR_AUTOMATION_H

#include <engine/shared/json.h>

#include <string>
#include <vector>

class CEditor;
bool RunEditorOperations(CEditor *pEditor, const json_value &Operations, std::vector<std::string> &vResults, std::string &Error);

#endif
