/**************************************************************************/
/*  perforce_context_menu.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "editor/inspector/editor_context_menu_plugin.h"

// "Perforce: Check Out / Mark for Add / Revert" entries in the right-click
// menus of the FileSystem dock, the Scene tree and the scene tabs.
class PerforceContextMenu : public EditorContextMenuPlugin {
	GDCLASS(PerforceContextMenu, EditorContextMenuPlugin);

public:
	enum Action {
		ACTION_CHECK_OUT,
		ACTION_ADD,
		ACTION_REVERT,
	};

private:
	ObjectID plugin; // PerforcePlugin; may be gone before menus are torn down.
	bool files; // FileSystem dock / scene tabs (paths) vs. Scene tree (nodes).

public:
	virtual void get_options(const Vector<String> &p_paths) override;

	PerforceContextMenu() {}
	PerforceContextMenu(ObjectID p_plugin, bool p_files) :
			plugin(p_plugin), files(p_files) {}
};
