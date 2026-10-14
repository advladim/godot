/**************************************************************************/
/*  perforce_context_menu.cpp                                             */
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

#include "perforce_context_menu.h"

#include "perforce_plugin.h"

#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"

void PerforceContextMenu::get_options(const Vector<String> &p_paths) {
	PerforcePlugin *p4 = ObjectDB::get_instance<PerforcePlugin>(plugin);
	if (!p4 || !p4->is_in_workspace()) {
		return;
	}
	if (files && p_paths.is_empty()) {
		return;
	}

	// The scene tree acts on the open scene's file (and instanced scenes).
	const String suffix = files ? String() : String(" ") + TTR("Scene");
	const Ref<Texture2D> checkout_icon = EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Edit"), EditorStringName(EditorIcons));
	const Ref<Texture2D> add_icon = EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Add"), EditorStringName(EditorIcons));
	const Ref<Texture2D> revert_icon = EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Close"), EditorStringName(EditorIcons));

	add_context_menu_item(TTR("Perforce: Check Out") + suffix, callable_mp(p4, &PerforcePlugin::context_action).bind(ACTION_CHECK_OUT), checkout_icon);
	if (files) {
		add_context_menu_item(TTR("Perforce: Mark for Add"), callable_mp(p4, &PerforcePlugin::context_action).bind(ACTION_ADD), add_icon);
	}
	add_context_menu_item(TTR("Perforce: Revert") + suffix, callable_mp(p4, &PerforcePlugin::context_action).bind(ACTION_REVERT), revert_icon);
}
