/**************************************************************************/
/*  perforce_dock.h                                                       */
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

#include "editor/docks/editor_dock.h"

class Button;
class HBoxContainer;
class Label;
class LineEdit;
class RichTextLabel;
class TextureRect;
class Tree;

// Bottom panel showing the Perforce connection, the files checked out in this
// project, and a log of everything the integration did.
// Pure UI: the plugin listens to its signals and feeds it data.
class PerforceDock : public EditorDock {
	GDCLASS(PerforceDock, EditorDock);

public:
	enum Status {
		STATUS_CONNECTING,
		STATUS_CONNECTED,
		STATUS_NOT_PERFORCE, // Project isn't inside a Perforce workspace.
		STATUS_LOGIN_REQUIRED,
		STATUS_OFFLINE, // Server unreachable.
		STATUS_NO_P4, // p4 command line client missing.
	};

	enum LogLevel {
		LOG_INFO,
		LOG_SUCCESS,
		LOG_WARNING,
		LOG_ERROR,
	};

private:
	Status status = STATUS_CONNECTING;
	String status_text;
	bool busy = false;
	bool has_log = false;
	int opened_count = 0;
	int outdated_count = 0;
	int unresolved_count = 0;

	TextureRect *status_icon = nullptr;
	Label *status_label = nullptr;
	Label *outdated_label = nullptr;
	Button *resolve_button = nullptr;
	Button *get_latest_button = nullptr;
	Button *refresh_button = nullptr;
	Button *connection_button = nullptr;

	HBoxContainer *setup_bar = nullptr; // Project setup problems, with buttons to fix them.
	TextureRect *setup_icon = nullptr;
	Label *setup_label = nullptr;
	Button *remove_cache_button = nullptr;
	Button *p4ignore_button = nullptr;

	Label *files_label = nullptr;
	Tree *files_tree = nullptr;
	Button *revert_button = nullptr;
	Button *revert_unchanged_button = nullptr;
	LineEdit *description_edit = nullptr;
	Button *submit_button = nullptr;

	RichTextLabel *log_text = nullptr;

	void _update_ui();
	void _emit(const StringName &p_signal);
	void _files_selected();
	void _files_activated();
	void _revert_pressed();
	void _submit_pressed();
	void _description_changed(const String &p_text);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_status(Status p_status, const String &p_text);
	Status get_status() const { return status; }
	void set_busy(bool p_busy, const String &p_text = String());

	// Each entry: { "path": res:// path, "action": "edit"/"add"/..., "type": filetype, "other": "bob@ws" or "" }.
	void set_opened_files(const Array &p_files);
	void set_remote_state(int p_outdated, int p_unresolved);
	// p_cache_files: number of .godot files in the depot. p_p4v_ignores: P4IGNORE is set, so P4V skips them too.
	void set_setup_state(int p_cache_files, bool p_p4v_ignores);
	void add_log(const String &p_message, LogLevel p_level);
	void clear_description();

	PerforceDock();
};

VARIANT_ENUM_CAST(PerforceDock::LogLevel);
