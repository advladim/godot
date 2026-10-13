/**************************************************************************/
/*  scene_merger.h                                                        */
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

#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

// Structure-aware three-way merge for Godot text resources (.tscn / .tres).
//
// The file is treated as sections ([ext_resource], [sub_resource], [node],
// [connection], ...) that are matched by identity (resource path/uid,
// sub-resource id, node path or unique_id, connection signature) instead of by
// line, so concurrent additions and edits to different nodes or properties
// merge cleanly. Only the same property changed differently on both sides (or
// a section edited on one side and deleted on the other) is a conflict.
//
// "Ours" is the local version (Perforce: yours), "theirs" the incoming one.
class SceneMerger {
public:
	enum Choice {
		CHOICE_OURS,
		CHOICE_THEIRS,
	};

	struct Conflict {
		String id; // Stable identifier, used to pass choices back in.
		String section; // Human readable, e.g. "Node Player/Sprite2D" or "SubResource RectangleShape2D_x1".
		String property; // Empty when the whole section conflicts (edited vs. deleted).
		String base, ours, theirs; // Raw values; empty when missing on that side.
		bool in_base = false, in_ours = false, in_theirs = false;
	};

	struct Result {
		String text;
		Vector<Conflict> conflicts;
		String error; // Non-empty if a file could not be parsed; text is then empty.
	};

	// Conflicts not listed in p_choices are resolved with p_default.
	static Result merge(const String &p_base, const String &p_ours, const String &p_theirs, const HashMap<String, Choice> &p_choices = HashMap<String, Choice>(), Choice p_default = CHOICE_OURS);

	// Entry point for `godot --merge-scene <base> <theirs> <yours> <result>`
	// (the argument order Perforce uses for merge tools). Returns the process exit code:
	// 0 = merged cleanly, 1 = merged with conflicts (local side kept), 2 = error.
	static int run_command_line(const Vector<String> &p_args);
};
