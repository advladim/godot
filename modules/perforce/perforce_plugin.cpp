/**************************************************************************/
/*  perforce_plugin.cpp                                                   */
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

#include "perforce_plugin.h"

#include "perforce_dock.h"
#include "perforce_resolve_dialog.h"
#include "perforce_write_hook.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "editor/editor_interface.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_toaster.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/main/timer.h"
#include "scene/scene_string_names.h"

// True if p_path is p_root or inside it.
static bool _is_inside(const String &p_path, const String &p_root) {
	String path = p_path.replace_char('\\', '/').simplify_path();
	String root = p_root.replace_char('\\', '/').simplify_path();
#ifdef WINDOWS_ENABLED
	path = path.to_lower();
	root = root.to_lower();
#endif
	if (root.is_empty()) {
		return false;
	}
	return path == root || path.begins_with(root.trim_suffix("/") + "/");
}

static String _get_setting(const String &p_key) {
	return EditorSettings::get_singleton()->get_project_metadata("perforce", p_key, String());
}

static bool _is_text_scene(const String &p_path) {
	const String ext = p_path.get_extension().to_lower();
	return ext == "tscn" || ext == "tres";
}

static String _local_to_res(const String &p_local_path) {
	return ProjectSettings::get_singleton()->localize_path(p_local_path.replace_char('\\', '/'));
}

void PerforcePlugin::_apply_connection_settings() {
	P4Connection conn;
	conn.p4_path = EDITOR_GET("version_control/perforce/p4_path");
	conn.port = _get_setting("port");
	conn.user = _get_setting("user");
	conn.client = _get_setting("client");
	conn.cwd = project_root;
	P4Client::set_connection(conn);
}

String PerforcePlugin::_depot_spec() const {
	return P4Client::escape(project_root) + "/...";
}

// --- Background tasks ---------------------------------------------------

bool PerforcePlugin::_start_task(const Callable &p_task, const String &p_busy_text) {
	if (task != WorkerThreadPool::INVALID_TASK_ID) {
		return false; // One thing at a time.
	}
	dock->set_busy(true, p_busy_text);
	task = WorkerThreadPool::get_singleton()->add_task(p_task, false, "Perforce");
	return true;
}

void PerforcePlugin::_finish_task() {
	if (task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task);
		task = WorkerThreadPool::INVALID_TASK_ID;
	}
	dock->set_busy(false);
	if (refresh_pending) {
		refresh_pending = false;
		_refresh();
	}
}

// --- Connecting ---------------------------------------------------------

void PerforcePlugin::_connect() {
	_apply_connection_settings();
	dock->set_status(PerforceDock::STATUS_CONNECTING, TTR("Connecting to Perforce..."));
	if (!_start_task(callable_mp(this, &PerforcePlugin::_task_connect), TTR("Connecting to Perforce..."))) {
		callable_mp(this, &PerforcePlugin::_connect).call_deferred();
	}
}

void PerforcePlugin::_task_connect() {
	Dictionary result;
	P4Connection conn = P4Client::get_connection();

	// If the .godot cache was ever submitted, its files are read-only; make them
	// all writable up front so no editor write ever fails on them.
	_unlock_cache_folder(project_root.path_join(".godot"));

	P4Result info = P4Client::run({ "info" });
	if (info.not_installed) {
		result["status"] = (int)(PerforceDock::STATUS_NO_P4);
		result["message"] = TTR("The Perforce command line client (p4) was not found. Install it with P4V (tick \"Command-Line Client\") or set its path in Editor Settings > Version Control > Perforce.");
	} else if (info.records.is_empty()) {
		result["status"] = (int)(info.connection_failed ? PerforceDock::STATUS_OFFLINE : PerforceDock::STATUS_NOT_PERFORCE);
		result["message"] = info.get_messages();
	} else {
		const P4Result::Record &rec = info.records[0];
		const String user = P4Result::field(rec, "userName");
		const String host = P4Result::field(rec, "clientHost");
		String workspace;

		if (_is_inside(project_root, P4Result::field(rec, "clientRoot"))) {
			workspace = P4Result::field(rec, "clientName");
		} else {
			// The current workspace doesn't contain the project; look for one of the user's workspaces that does.
			P4Result clients = P4Client::run({ "clients", "-u", user });
			if (clients.login_required) {
				result["status"] = (int)(PerforceDock::STATUS_LOGIN_REQUIRED);
				result["message"] = TTR("Log in to Perforce to find the workspace for this project.");
			}
			for (const P4Result::Record &client : clients.records) {
				const String client_host = P4Result::field(client, "Host");
				if ((client_host.is_empty() || client_host.nocasecmp_to(host) == 0) && _is_inside(project_root, P4Result::field(client, "Root"))) {
					workspace = P4Result::field(client, "client");
					conn.client = workspace;
					P4Client::set_connection(conn);
					result["save_client"] = workspace;
					break;
				}
			}
		}

		result["workspace"] = workspace;
		result["user"] = user;
		result["server"] = conn.port.is_empty() ? P4Client::get_variable("P4PORT") : conn.port;

		if (workspace.is_empty()) {
			if (!result.has("status")) {
				result["status"] = (int)(PerforceDock::STATUS_NOT_PERFORCE);
			}
		} else if (P4Client::run({ "login", "-s" }).login_required) {
			result["status"] = (int)(PerforceDock::STATUS_LOGIN_REQUIRED);
			result["message"] = vformat(TTR("Log in to Perforce to use workspace \"%s\"."), workspace);
		} else {
			result["status"] = (int)(PerforceDock::STATUS_CONNECTED);
		}
	}
	callable_mp(this, &PerforcePlugin::_connect_done).call_deferred(result);
}

void PerforcePlugin::_connect_done(const Dictionary &p_result) {
	_finish_task();

	const PerforceDock::Status status = (PerforceDock::Status)(int)p_result["status"];
	const String message = p_result.get("message", String());
	workspace_name = p_result.get("workspace", String());
	if (p_result.has("save_client")) {
		// Remember it for this project so every command uses it.
		EditorSettings::get_singleton()->set_project_metadata("perforce", "client", p_result["save_client"]);
	}

	connected = status == PerforceDock::STATUS_CONNECTED;
	if (workspace_name.is_empty()) {
		_remove_hook();
	} else {
		_install_hook(); // Also when logged out: it will ask for a login when it needs one.
		if (status == PerforceDock::STATUS_CONNECTED) {
			_ensure_p4ignore();
		}
	}

	switch (status) {
		case PerforceDock::STATUS_CONNECTED: {
			dock->set_status(status, vformat(TTR("Connected: workspace %s, user %s, server %s"), workspace_name, p_result["user"], p_result["server"]));
			dock->add_log(vformat(TTR("Connected to workspace %s. Files are checked out automatically when you save."), workspace_name), PerforceDock::LOG_SUCCESS);
			poll_timer->start();
			_refresh();
		} break;
		case PerforceDock::STATUS_LOGIN_REQUIRED: {
			dock->set_status(status, TTR("Not logged in. Click the settings button to log in."));
			dock->add_log(message, PerforceDock::LOG_WARNING);
			_show_connection_dialog(message);
		} break;
		case PerforceDock::STATUS_OFFLINE: {
			dock->set_status(status, vformat(TTR("Can't reach the Perforce server %s. Files can't be checked out."), p_result.get("server", String())));
			dock->add_log(message, PerforceDock::LOG_ERROR);
		} break;
		case PerforceDock::STATUS_NO_P4: {
			dock->set_status(status, TTR("Perforce command line client (p4) not found."));
			dock->add_log(message, PerforceDock::LOG_ERROR);
		} break;
		default: {
			dock->set_status(PerforceDock::STATUS_NOT_PERFORCE, TTR("This project is not in a Perforce workspace."));
			if (!message.is_empty()) {
				dock->add_log(message, PerforceDock::LOG_INFO);
			}
		} break;
	}
}

// --- Refreshing ---------------------------------------------------------

void PerforcePlugin::_refresh() {
	if (!connected) {
		return;
	}
	if (!_start_task(callable_mp(this, &PerforcePlugin::_task_refresh), String())) {
		refresh_pending = true;
	}
}

void PerforcePlugin::_task_refresh() {
	Dictionary result;
	const String spec = _depot_spec();

	// Files opened in this project.
	P4Result opened = P4Client::run({ "fstat", "-Ro", spec });
	result["login_required"] = opened.login_required;
	Array files;
	for (const P4Result::Record &rec : opened.records) {
		const String action = P4Result::field(rec, "action");
		if (action == "move/delete") {
			continue; // Shown as its move/add counterpart.
		}
		Dictionary file;
		file["path"] = _local_to_res(P4Result::field(rec, "clientFile"));
		file["action"] = action;
		file["type"] = P4Result::field(rec, "type");
		file["other"] = P4Result::field(rec, "otherOpen0");
		files.push_back(file);
	}
	result["files"] = files;

	// What teammates submitted since our last sync, and what still needs merging.
	result["outdated"] = P4Client::run({ "sync", "-n", spec }).records.size();
	result["unresolved"] = P4Client::run({ "resolve", "-n", spec }).records.size();

	// Project setup problems.
	result["cache_files"] = P4Client::run({ "files", "-e", P4Client::escape(project_root) + "/.godot/..." }).records.size();
	result["p4v_ignores"] = !P4Client::get_variable("P4IGNORE").is_empty();

	callable_mp(this, &PerforcePlugin::_refresh_done).call_deferred(result);
}

void PerforcePlugin::_refresh_done(const Dictionary &p_result) {
	_finish_task();
	if ((bool)p_result["login_required"]) {
		_login_required(String());
		return;
	}
	dock->set_opened_files(p_result["files"]);
	dock->set_remote_state(p_result["outdated"], p_result["unresolved"]);
	dock->set_setup_state(p_result["cache_files"], p_result["p4v_ignores"]);
}

// --- Get latest and resolving -------------------------------------------

void PerforcePlugin::_get_latest() {
	EditorInterface::get_singleton()->save_all_scenes();
	if (hook) {
		hook->flush_pending_adds();
	}
	_start_task(callable_mp(this, &PerforcePlugin::_task_get_latest), TTR("Getting latest from the server..."));
}

void PerforcePlugin::_task_get_latest() {
	Dictionary result;
	P4Result sync = P4Client::run({ "sync", _depot_spec() });
	PackedStringArray updated;
	for (const P4Result::Record &rec : sync.records) {
		updated.push_back(_local_to_res(P4Result::field(rec, "clientFile")));
	}
	// Files you have checked out aren't overwritten; they're scheduled for merging instead.
	int to_merge = 0;
	for (const String &info : sync.infos) {
		if (info.contains("must resolve")) {
			to_merge++;
		}
	}
	result["to_merge"] = to_merge;
	result["updated"] = updated;
	result["errors"] = sync.errors.is_empty() ? String() : sync.get_messages();
	result["login_required"] = sync.login_required;
	callable_mp(this, &PerforcePlugin::_get_latest_done).call_deferred(result);
}

void PerforcePlugin::_get_latest_done(const Dictionary &p_result) {
	_finish_task();
	if ((bool)p_result["login_required"]) {
		_login_required(String());
		return;
	}

	const PackedStringArray updated = p_result["updated"];
	const String errors = p_result["errors"];
	if (!errors.is_empty()) {
		dock->add_log(TTR("Get Latest:") + " " + errors, PerforceDock::LOG_ERROR);
	}
	const int to_merge = p_result["to_merge"];
	if (to_merge > 0) {
		dock->add_log(vformat(TTRN("%d file you're editing also changed on the server, merging...", "%d files you're editing also changed on the server, merging...", to_merge), to_merge), PerforceDock::LOG_INFO);
	}
	if (updated.is_empty() && to_merge == 0) {
		dock->add_log(TTR("Already up to date."), PerforceDock::LOG_SUCCESS);
	} else if (!updated.is_empty()) {
		dock->add_log(vformat(TTRN("Got %d updated file from the server.", "Got %d updated files from the server.", updated.size()), updated.size()), PerforceDock::LOG_SUCCESS);
		EditorFileSystem::get_singleton()->scan_changes();
		_reload_changed_scenes(updated);
	}
	// Files you had checked out that changed on the server need merging.
	_resolve();
}

void PerforcePlugin::_resolve() {
	if (!_start_task(callable_mp(this, &PerforcePlugin::_task_resolve), TTR("Merging server changes..."))) {
		refresh_pending = true;
	}
}

void PerforcePlugin::_task_resolve() {
	Dictionary result;
	const String spec = _depot_spec();

	// Accept the server version where only it changed, and ours where only we changed.
	P4Client::run({ "resolve", "-as", spec });

	P4Result pending = P4Client::run({ "resolve", "-n", spec });
	Array scenes;
	PackedStringArray merged, failed;
	for (const P4Result::Record &rec : pending.records) {
		const String local = P4Result::field(rec, "clientFile");
		const String res_path = _local_to_res(local);
		if (P4Result::field(rec, "resolveType") != "content") {
			failed.push_back(res_path); // Renames, deletes, filetype changes: leave to P4V.
			continue;
		}

		if (_is_text_scene(local)) {
			// Scenes: merge by structure, a line merge can produce broken scenes.
			String base, theirs;
			// The common ancestor; for a plain sync it's the revision we had.
			const String base_rev = rec.has("baseFile") ? P4Result::field(rec, "baseFile") + "#" + P4Result::field(rec, "baseRev") : P4Result::field(rec, "fromFile") + "#" + P4Result::field(rec, "startFromRev");
			Error base_err = P4Client::print(base_rev, base);
			Error theirs_err = P4Client::print(P4Result::field(rec, "fromFile") + "#" + P4Result::field(rec, "endFromRev"), theirs);
			Error yours_err = OK;
			String yours = FileAccess::get_file_as_string(local, &yours_err);
			if (base_err != OK || theirs_err != OK || yours_err != OK) {
				failed.push_back(res_path);
				continue;
			}
			Dictionary scene;
			scene["file"] = res_path;
			scene["local"] = local;
			scene["base"] = base.replace("\r", "");
			scene["theirs"] = theirs.replace("\r", "");
			scene["yours"] = yours.replace("\r", "");
			scenes.push_back(scene);
		} else {
			// Other text files: Perforce's own merge, only if it has no conflicts.
			P4Result auto_merge = P4Client::run({ "resolve", "-am", P4Client::escape(local) });
			if (auto_merge.ok() && P4Client::run({ "resolve", "-n", P4Client::escape(local) }).records.is_empty()) {
				merged.push_back(res_path);
			} else {
				failed.push_back(res_path);
			}
		}
	}
	result["scenes"] = scenes;
	result["merged"] = merged;
	result["failed"] = failed;
	callable_mp(this, &PerforcePlugin::_resolve_done).call_deferred(result);
}

void PerforcePlugin::_resolve_done(const Dictionary &p_result) {
	_finish_task();

	const PackedStringArray merged = p_result["merged"];
	const PackedStringArray failed = p_result["failed"];
	for (const String &file : merged) {
		dock->add_log(vformat(TTR("Merged server changes into %s."), file.trim_prefix("res://")), PerforceDock::LOG_SUCCESS);
	}
	if (!failed.is_empty()) {
		dock->add_log(vformat(TTR("These files need to be resolved in P4V: %s"), String(", ").join(failed)), PerforceDock::LOG_WARNING);
	}
	_reload_changed_scenes(merged);

	pending_scene_merges.append_array(p_result["scenes"]);
	_merge_next_scene();
}

// Merges queued scenes one by one; stops to show the conflict dialog when needed.
void PerforcePlugin::_merge_next_scene() {
	while (!pending_scene_merges.is_empty()) {
		Dictionary scene = pending_scene_merges[0];
		SceneMerger::Result merge = SceneMerger::merge(scene["base"], scene["yours"], scene["theirs"]);
		if (!merge.error.is_empty()) {
			dock->add_log(vformat(TTR("Couldn't merge %s automatically (%s). Resolve it in P4V."), scene["file"], merge.error), PerforceDock::LOG_ERROR);
			pending_scene_merges.pop_front();
			continue;
		}
		if (!merge.conflicts.is_empty()) {
			resolve_dialog->open(scene["file"], merge.conflicts);
			return; // Continues in _scene_conflicts_resolved().
		}
		_apply_scene_merge(scene["file"], merge.text);
		pending_scene_merges.pop_front();
	}
	_refresh();
}

void PerforcePlugin::_scene_conflicts_resolved(const String &p_file, const Dictionary &p_use_theirs) {
	ERR_FAIL_COND(pending_scene_merges.is_empty());
	Dictionary scene = pending_scene_merges[0];
	ERR_FAIL_COND(String(scene["file"]) != p_file);

	HashMap<String, SceneMerger::Choice> choices;
	const Array ids = p_use_theirs.keys();
	for (const Variant &id : ids) {
		choices[id] = (bool)p_use_theirs[id] ? SceneMerger::CHOICE_THEIRS : SceneMerger::CHOICE_OURS;
	}
	SceneMerger::Result merge = SceneMerger::merge(scene["base"], scene["yours"], scene["theirs"], choices);
	_apply_scene_merge(p_file, merge.text);
	pending_scene_merges.pop_front();
	_merge_next_scene();
}

bool PerforcePlugin::_apply_scene_merge(const String &p_file, const String &p_text) {
	const String local = ProjectSettings::get_singleton()->globalize_path(p_file);
	// Mark it resolved keeping the local file, then put the merged result in it.
	P4Result accept = P4Client::run({ "resolve", "-ay", P4Client::escape(local) });
	if (!accept.ok()) {
		dock->add_log(vformat(TTR("Couldn't mark %s as resolved: %s"), p_file, accept.get_messages()), PerforceDock::LOG_ERROR);
		return false;
	}
	Ref<FileAccess> file = FileAccess::open(local, FileAccess::WRITE);
	if (file.is_null()) {
		dock->add_log(vformat(TTR("Couldn't write the merged %s."), p_file), PerforceDock::LOG_ERROR);
		return false;
	}
	file->store_string(p_text);
	file.unref();

	dock->add_log(vformat(TTR("Merged %s: kept your changes and the server's."), p_file.trim_prefix("res://")), PerforceDock::LOG_SUCCESS);
	EditorFileSystem::get_singleton()->update_file(p_file);
	_reload_changed_scenes({ p_file });
	return true;
}

void PerforcePlugin::_reload_changed_scenes(const PackedStringArray &p_paths) {
	const PackedStringArray open_scenes = EditorInterface::get_singleton()->get_open_scenes();
	for (const String &path : p_paths) {
		if (open_scenes.has(path)) {
			EditorInterface::get_singleton()->reload_scene_from_path(path);
		}
	}
}

// --- Submit and revert --------------------------------------------------

void PerforcePlugin::_submit(const String &p_description) {
	_confirm(TTR("Submit"), TTR("Save all scenes and submit your checked out files in this project to the server?"), TTR("Submit"), callable_mp(this, &PerforcePlugin::_task_submit).bind(p_description));
	confirm_is_submit = true;
}

void PerforcePlugin::_task_submit(const String &p_description) {
	Dictionary result;
	P4Result submit = P4Client::run({ "submit", "-d", p_description, _depot_spec() });
	String change;
	for (const P4Result::Record &rec : submit.records) {
		if (rec.has("submittedChange")) {
			change = P4Result::field(rec, "submittedChange");
		}
	}
	result["ok"] = submit.ok() && !change.is_empty();
	result["change"] = change;
	result["messages"] = submit.get_messages();
	result["login_required"] = submit.login_required;
	callable_mp(this, &PerforcePlugin::_submit_done).call_deferred(result);
}

void PerforcePlugin::_submit_done(const Dictionary &p_result) {
	_finish_task();
	if ((bool)p_result["login_required"]) {
		_login_required(String());
		return;
	}
	if ((bool)p_result["ok"]) {
		dock->add_log(vformat(TTR("Submitted change %s."), p_result["change"]), PerforceDock::LOG_SUCCESS);
		dock->clear_description();
	} else {
		String messages = p_result["messages"];
		if (messages.contains("must resolve") || messages.contains("out of date")) {
			messages += " " + TTR("Click Get Latest first, then submit again.");
		}
		dock->add_log(TTR("Submit failed:") + " " + messages, PerforceDock::LOG_ERROR);
		EditorToaster::get_singleton()->popup_str(TTR("Perforce submit failed. See the Perforce panel."), EditorToaster::SEVERITY_ERROR);
	}
	_refresh();
}

void PerforcePlugin::_revert(const PackedStringArray &p_paths) {
	_confirm(TTR("Revert"), vformat(TTRN("Discard your changes to %d file? This can't be undone.", "Discard your changes to %d files? This can't be undone.", p_paths.size()), p_paths.size()), TTR("Revert"), callable_mp(this, &PerforcePlugin::_task_revert).bind(p_paths));
}

void PerforcePlugin::_task_revert(const PackedStringArray &p_paths) {
	Vector<String> files;
	for (const String &path : p_paths) {
		files.push_back(P4Client::escape(ProjectSettings::get_singleton()->globalize_path(path)));
	}
	P4Result revert = P4Client::run_files({ "revert" }, files);
	Dictionary result;
	result["paths"] = p_paths;
	result["errors"] = revert.errors.is_empty() ? String() : revert.get_messages();
	callable_mp(this, &PerforcePlugin::_revert_done).call_deferred(result);
}

void PerforcePlugin::_revert_done(const Dictionary &p_result) {
	_finish_task();
	const PackedStringArray paths = p_result["paths"];
	const String errors = p_result["errors"];
	if (errors.is_empty()) {
		dock->add_log(vformat(TTRN("Reverted %d file.", "Reverted %d files.", paths.size()), paths.size()), PerforceDock::LOG_INFO);
	} else {
		dock->add_log(TTR("Revert:") + " " + errors, PerforceDock::LOG_ERROR);
	}
	EditorFileSystem::get_singleton()->scan_changes();
	_reload_changed_scenes(paths);
	_refresh();
}

// --- Keeping the .godot cache out of Perforce ------------------------------

// Every Perforce project needs a .p4ignore listing .godot/; create or fix it.
// New files are marked for add by the hook like any other file.
void PerforcePlugin::_ensure_p4ignore() {
	const String path = project_root.path_join(".p4ignore");
	String text;
	if (FileAccess::exists(path)) {
		text = FileAccess::get_file_as_string(path);
		for (const String &line : text.replace("\r", "").split("\n")) {
			const String entry = line.strip_edges();
			if (entry == ".godot" || entry == ".godot/" || entry == "/.godot/" || entry == ".godot/*" || entry == ".godot/**") {
				return; // Already ignored.
			}
		}
		if (!text.is_empty() && !text.ends_with("\n")) {
			text += "\n";
		}
	} else {
		text = "# Files Perforce should never add. Godot's .godot folder is a local cache.\n";
	}
	text += ".godot/\n";

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		dock->add_log(TTR("Couldn't write .p4ignore."), PerforceDock::LOG_WARNING);
		return;
	}
	file->store_string(text);
	file.unref();
	dock->add_log(TTR("Added .godot/ to .p4ignore so Perforce never adds the editor cache. Submit .p4ignore so your team gets it."), PerforceDock::LOG_SUCCESS);
}

void PerforcePlugin::_remove_cache() {
	_confirm(TTR("Remove .godot from Perforce"),
			TTR("This submits a change that deletes the .godot folder from the server.\n\nYour own .godot files stay on disk. Teammates' editors rebuild theirs automatically after they Get Latest (the first project open may take a little longer while assets reimport).\n\n.p4ignore is submitted with it so the folder isn't added again."),
			TTR("Remove and Submit"), callable_mp(this, &PerforcePlugin::_task_remove_cache));
}

void PerforcePlugin::_task_remove_cache() {
	Dictionary result;
	const String cache_spec = P4Client::escape(project_root) + "/.godot/...";
	const String p4ignore = P4Client::escape(project_root.path_join(".p4ignore"));

	// -k: remove from the depot but keep the local files.
	P4Result del = P4Client::run({ "delete", "-k", cache_spec });

	// `p4 submit` takes a single file pattern, so the cache removal and the new
	// .p4ignore go in as two changes.
	P4Result submit = P4Client::run({ "submit", "-d", "Remove the .godot editor cache from Perforce (it is local and regenerated by Godot).", cache_spec });
	String change;
	for (const P4Result::Record &rec : submit.records) {
		if (rec.has("submittedChange")) {
			change = P4Result::field(rec, "submittedChange");
		}
	}
	if (!change.is_empty() && !P4Client::run({ "fstat", "-Ro", p4ignore }).records.is_empty()) {
		P4Client::run({ "submit", "-d", "Add .p4ignore so the .godot editor cache is never added again.", p4ignore });
	}
	result["change"] = change;
	result["messages"] = del.errors.is_empty() ? submit.get_messages() : del.get_messages();
	result["login_required"] = del.login_required || submit.login_required;
	callable_mp(this, &PerforcePlugin::_remove_cache_done).call_deferred(result);
}

void PerforcePlugin::_remove_cache_done(const Dictionary &p_result) {
	_finish_task();
	if ((bool)p_result["login_required"]) {
		_login_required(String());
		return;
	}
	const String change = p_result["change"];
	if (change.is_empty()) {
		dock->add_log(TTR("Removing .godot from Perforce failed:") + " " + String(p_result["messages"]), PerforceDock::LOG_ERROR);
	} else {
		dock->add_log(vformat(TTR("Removed the .godot folder from Perforce (change %s). Tell your team to Get Latest."), change), PerforceDock::LOG_SUCCESS);
	}
	_unlock_cache_folder(project_root.path_join(".godot"));
	_refresh();
}

// Makes every file in the cache folder writable again.
void PerforcePlugin::_unlock_cache_folder(const String &p_dir) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}
	for (const String &file : da->get_files()) {
		const String path = p_dir.path_join(file);
		if (FileAccess::get_read_only_attribute(path)) {
			FileAccess::set_read_only_attribute(path, false);
		}
	}
	for (const String &dir : da->get_directories()) {
		_unlock_cache_folder(p_dir.path_join(dir));
	}
}

void PerforcePlugin::_enable_p4ignore() {
	_confirm(TTR("Make P4V Use .p4ignore"),
			TTR("This runs \"p4 set P4IGNORE=.p4ignore\" for your Windows user, so P4V and the p4 command line skip files listed in any .p4ignore file (like the .godot folder).\n\nIt only affects projects that have a .p4ignore file. Restart P4V afterwards."),
			TTR("Set P4IGNORE"), Callable());
	confirm_is_p4ignore = true;
}

void PerforcePlugin::_confirm(const String &p_title, const String &p_text, const String &p_ok, const Callable &p_action) {
	confirm_dialog->set_title(p_title);
	confirm_dialog->set_text(p_text);
	confirm_dialog->set_ok_button_text(p_ok);
	confirm_action = p_action;
	confirm_is_submit = false;
	confirm_is_p4ignore = false;
	confirm_dialog->popup_centered();
}

void PerforcePlugin::_confirmed() {
	if (confirm_is_p4ignore) {
		confirm_is_p4ignore = false;
		if (P4Client::set_variable("P4IGNORE", ".p4ignore") == OK) {
			dock->add_log(TTR("P4IGNORE is set. Restart P4V so it picks it up."), PerforceDock::LOG_SUCCESS);
		} else {
			dock->add_log(TTR("Couldn't run \"p4 set P4IGNORE=.p4ignore\"."), PerforceDock::LOG_ERROR);
		}
		_refresh();
		return;
	}
	if (confirm_action.is_null()) {
		return;
	}
	if (confirm_is_submit) {
		EditorInterface::get_singleton()->save_all_scenes();
		if (hook) {
			hook->flush_pending_adds(); // New files must be marked for add before submitting.
		}
	}
	_start_task(confirm_action, TTR("Talking to the server..."));
	confirm_action = Callable();
}

// --- Hook ----------------------------------------------------------------

void PerforcePlugin::_install_hook() {
	if (hook) {
		return;
	}
	hook = memnew(PerforceWriteHook(project_root, callable_mp(this, &PerforcePlugin::_login_required), callable_mp(this, &PerforcePlugin::_activity)));
	FileSystemWriteHook::set_singleton(hook);
	if (flush_timer->is_inside_tree()) {
		flush_timer->start();
	}
}

void PerforcePlugin::_remove_hook() {
	if (!hook) {
		return;
	}
	flush_timer->stop();
	hook->flush_pending_adds();
	if (FileSystemWriteHook::get_singleton() == hook) {
		FileSystemWriteHook::set_singleton(nullptr);
	}
	memdelete(hook);
	hook = nullptr;
}

void PerforcePlugin::_activity(const String &p_message, int p_level) {
	dock->add_log(p_message, (PerforceDock::LogLevel)p_level);
	print_line("Perforce: " + p_message);
	if (p_level == PerforceDock::LOG_ERROR) {
		EditorToaster::get_singleton()->popup_str("Perforce: " + p_message, EditorToaster::SEVERITY_WARNING);
	}
	refresh_timer->start(); // Update the file list once things settle down.
}

void PerforcePlugin::_flush_pending_adds() {
	if (hook) {
		hook->flush_pending_adds();
	}
}

// --- Connection dialog ---------------------------------------------------

void PerforcePlugin::_show_connection_dialog(const String &p_message) {
	if (connection_dialog->is_visible()) {
		if (!p_message.is_empty()) {
			connection_message->set_text(p_message);
		}
		return;
	}

	// Empty fields use the same settings as P4V / `p4 set`; show those as hints.
	port_edit->set_text(_get_setting("port"));
	port_edit->set_placeholder(P4Client::get_variable("P4PORT"));
	user_edit->set_text(_get_setting("user"));
	user_edit->set_placeholder(P4Client::get_variable("P4USER"));
	client_edit->set_text(_get_setting("client"));
	client_edit->set_placeholder(workspace_name.is_empty() ? P4Client::get_variable("P4CLIENT") : workspace_name);
	password_edit->clear();

	connection_message->set_text(p_message);
	connection_message->set_visible(!p_message.is_empty());
	connection_dialog->popup_centered(Size2(500, 0) * EDSCALE);
	password_edit->grab_focus();
}

void PerforcePlugin::_connection_dialog_confirmed() {
	EditorSettings::get_singleton()->set_project_metadata("perforce", "port", port_edit->get_text().strip_edges());
	EditorSettings::get_singleton()->set_project_metadata("perforce", "user", user_edit->get_text().strip_edges());
	EditorSettings::get_singleton()->set_project_metadata("perforce", "client", client_edit->get_text().strip_edges());
	_apply_connection_settings();

	const String password = password_edit->get_text();
	password_edit->clear();
	if (!password.is_empty()) {
		P4Result result = P4Client::login(password);
		if (!result.ok()) {
			dock->add_log(TTR("Login failed:") + " " + result.get_messages(), PerforceDock::LOG_ERROR);
			callable_mp(this, &PerforcePlugin::_show_connection_dialog).call_deferred(TTR("Login failed.") + "\n" + result.get_messages());
			return;
		}
		dock->add_log(TTR("Logged in."), PerforceDock::LOG_SUCCESS);
	}
	_connect();
}

void PerforcePlugin::_login_required(const String &p_message) {
	if (connected) {
		connected = false;
		dock->set_status(PerforceDock::STATUS_LOGIN_REQUIRED, TTR("Your Perforce login expired. Click the settings button to log in."));
		dock->add_log(TTR("Your Perforce login expired. Log in, then save again."), PerforceDock::LOG_WARNING);
	}
	_show_connection_dialog(TTR("Your Perforce login has expired. Log in, then save again.") + (p_message.is_empty() ? String() : "\n" + p_message));
}

// --- Setup ------------------------------------------------------------

void PerforcePlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			project_root = ProjectSettings::get_singleton()->get_resource_path().replace_char('\\', '/').simplify_path();
			add_dock(dock);
			EditorInterface::get_singleton()->get_base_control()->add_child(connection_dialog);
			EditorInterface::get_singleton()->get_base_control()->add_child(confirm_dialog);
			EditorInterface::get_singleton()->get_base_control()->add_child(resolve_dialog);
			add_child(flush_timer);
			add_child(refresh_timer);
			add_child(poll_timer);
			if (hook) {
				flush_timer->start();
			}
			callable_mp(this, &PerforcePlugin::_connect).call_deferred();
		} break;
		case NOTIFICATION_APPLICATION_FOCUS_IN: {
			// Coming back from P4V or elsewhere: show the current state.
			_refresh();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (task != WorkerThreadPool::INVALID_TASK_ID) {
				WorkerThreadPool::get_singleton()->wait_for_task_completion(task);
				task = WorkerThreadPool::INVALID_TASK_ID;
			}
			// The hook stays installed until the plugin is destroyed: the editor
			// still saves files (project metadata, layout) while shutting down.
			if (hook) {
				hook->flush_pending_adds();
			}
			remove_dock(dock);
		} break;
	}
}

PerforcePlugin::PerforcePlugin() {
	EDITOR_DEF("version_control/perforce/p4_path", "p4");
	project_root = ProjectSettings::get_singleton()->get_resource_path().replace_char('\\', '/').simplify_path();

	dock = memnew(PerforceDock);
	dock->connect("refresh_requested", callable_mp(this, &PerforcePlugin::_connect));
	dock->connect("connection_requested", callable_mp(this, &PerforcePlugin::_show_connection_dialog).bind(String()));
	dock->connect("get_latest_requested", callable_mp(this, &PerforcePlugin::_get_latest));
	dock->connect("resolve_requested", callable_mp(this, &PerforcePlugin::_resolve));
	dock->connect("revert_requested", callable_mp(this, &PerforcePlugin::_revert));
	dock->connect("submit_requested", callable_mp(this, &PerforcePlugin::_submit));
	dock->connect("remove_cache_requested", callable_mp(this, &PerforcePlugin::_remove_cache));
	dock->connect("enable_p4ignore_requested", callable_mp(this, &PerforcePlugin::_enable_p4ignore));

	flush_timer = memnew(Timer);
	flush_timer->set_wait_time(0.5);
	flush_timer->connect("timeout", callable_mp(this, &PerforcePlugin::_flush_pending_adds));

	refresh_timer = memnew(Timer);
	refresh_timer->set_wait_time(1.5);
	refresh_timer->set_one_shot(true);
	refresh_timer->connect("timeout", callable_mp(this, &PerforcePlugin::_refresh));

	poll_timer = memnew(Timer);
	poll_timer->set_wait_time(120);
	poll_timer->connect("timeout", callable_mp(this, &PerforcePlugin::_refresh));

	confirm_dialog = memnew(ConfirmationDialog);
	confirm_dialog->connect(SceneStringName(confirmed), callable_mp(this, &PerforcePlugin::_confirmed));

	resolve_dialog = memnew(PerforceResolveDialog);
	resolve_dialog->connect("resolved", callable_mp(this, &PerforcePlugin::_scene_conflicts_resolved));
	resolve_dialog->connect("canceled", callable_mp(this, &PerforcePlugin::_refresh));

	connection_dialog = memnew(ConfirmationDialog);
	connection_dialog->set_title(TTR("Perforce Connection"));
	connection_dialog->set_ok_button_text(TTR("Connect"));
	connection_dialog->connect(SceneStringName(confirmed), callable_mp(this, &PerforcePlugin::_connection_dialog_confirmed));

	VBoxContainer *vbox = memnew(VBoxContainer);
	connection_dialog->add_child(vbox);

	connection_message = memnew(Label);
	connection_message->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	vbox->add_child(connection_message);

	GridContainer *grid = memnew(GridContainer);
	grid->set_columns(2);
	vbox->add_child(grid);

	auto add_field = [&](const String &p_label) {
		Label *label = memnew(Label(p_label));
		grid->add_child(label);
		LineEdit *edit = memnew(LineEdit);
		edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		grid->add_child(edit);
		return edit;
	};
	port_edit = add_field(TTR("Server:"));
	user_edit = add_field(TTR("User:"));
	client_edit = add_field(TTR("Workspace:"));
	password_edit = add_field(TTR("Password:"));
	password_edit->set_secret(true);
	password_edit->set_placeholder(TTR("Only needed when not logged in"));
	connection_dialog->register_text_enter(password_edit);

	Label *hint = memnew(Label(TTR("Leave fields empty to use the same settings as P4V. Your password is sent to \"p4 login\" and never stored.")));
	hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	hint->set_modulate(Color(1, 1, 1, 0.65));
	vbox->add_child(hint);

	// Install the hook now: the editor writes its caches (and maybe project.godot)
	// while loading, before plugins enter the tree or the background connection
	// check finishes. It's removed again if the project isn't in a Perforce workspace.
	_apply_connection_settings();
	_install_hook();
}

PerforcePlugin::~PerforcePlugin() {
	_remove_hook();
	for (Node *node : { (Node *)dock, (Node *)connection_dialog, (Node *)confirm_dialog, (Node *)resolve_dialog, (Node *)flush_timer, (Node *)refresh_timer, (Node *)poll_timer }) {
		if (!node->is_inside_tree() && !node->get_parent()) {
			memdelete(node);
		}
	}
}
