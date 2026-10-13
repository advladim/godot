/**************************************************************************/
/*  perforce_plugin.h                                                     */
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

#include "p4_client.h"
#include "scene_merger.h"

#include "core/object/worker_thread_pool.h"
#include "editor/plugins/editor_plugin.h"

class ConfirmationDialog;
class Label;
class LineEdit;
class PerforceDock;
class PerforceResolveDialog;
class PerforceWriteHook;
class Timer;

// Connects the editor to the Perforce workspace that contains the project,
// installs PerforceWriteHook so files are checked out/added/moved
// automatically, and drives the Perforce dock (status, changelist, submit,
// get latest, scene merging).
//
// Every p4 call made from here runs on a worker thread, one at a time, so the
// editor never freezes waiting for the server.
class PerforcePlugin : public EditorPlugin {
	GDCLASS(PerforcePlugin, EditorPlugin);

	PerforceDock *dock = nullptr;
	PerforceWriteHook *hook = nullptr;
	Timer *flush_timer = nullptr;
	Timer *refresh_timer = nullptr; // Debounces refreshes after activity.
	Timer *poll_timer = nullptr; // Periodically checks for newer files on the server.

	ConfirmationDialog *connection_dialog = nullptr;
	Label *connection_message = nullptr;
	LineEdit *port_edit = nullptr;
	LineEdit *user_edit = nullptr;
	LineEdit *client_edit = nullptr;
	LineEdit *password_edit = nullptr;

	ConfirmationDialog *confirm_dialog = nullptr;
	Callable confirm_action;
	bool confirm_is_submit = false;
	bool confirm_is_p4ignore = false;
	PerforceResolveDialog *resolve_dialog = nullptr;

	bool connected = false;
	String project_root;
	String workspace_name;

	// Background work.
	WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	bool refresh_pending = false;

	// Scene files waiting for the conflict dialog. Each: { "file", "base", "yours", "theirs" }.
	Array pending_scene_merges;

	void _apply_connection_settings();
	String _depot_spec() const; // Project folder as a p4 file spec.

	bool _start_task(const Callable &p_task, const String &p_busy_text);
	void _finish_task();

	void _connect();
	void _task_connect();
	void _connect_done(const Dictionary &p_result);

	void _refresh();
	void _task_refresh();
	void _refresh_done(const Dictionary &p_result);

	void _get_latest();
	void _task_get_latest();
	void _get_latest_done(const Dictionary &p_result);

	void _resolve();
	void _task_resolve();
	void _resolve_done(const Dictionary &p_result);
	void _merge_next_scene();
	void _scene_conflicts_resolved(const String &p_file, const Dictionary &p_use_theirs);
	bool _apply_scene_merge(const String &p_file, const String &p_text);

	void _submit(const String &p_description);
	void _task_submit(const String &p_description);
	void _submit_done(const Dictionary &p_result);

	void _revert(const PackedStringArray &p_paths);
	void _task_revert(const PackedStringArray &p_paths);
	void _revert_done(const Dictionary &p_result);

	void _ensure_p4ignore();
	void _remove_cache();
	void _task_remove_cache();
	void _remove_cache_done(const Dictionary &p_result);
	void _enable_p4ignore();
	void _unlock_cache_folder(const String &p_dir);

	void _install_hook();
	void _remove_hook();
	void _reload_changed_scenes(const PackedStringArray &p_paths);

	void _confirm(const String &p_title, const String &p_text, const String &p_ok, const Callable &p_action);
	void _confirmed();

	void _show_connection_dialog(const String &p_message = String());
	void _connection_dialog_confirmed();
	void _login_required(const String &p_message);
	void _activity(const String &p_message, int p_level);
	void _flush_pending_adds();

protected:
	void _notification(int p_what);

public:
	virtual String get_plugin_name() const override { return "Perforce"; }

	PerforcePlugin();
	~PerforcePlugin();
};
