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

#include "perforce_context_menu.h"
#include "perforce_dock.h"
#include "perforce_resolve_dialog.h"
#include "perforce_review_dialog.h"
#include "perforce_write_hook.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_interface.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_toaster.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/popup_menu.h"
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

// Files the structure-aware merger handles (instead of Perforce's line merge).
static bool _is_text_scene(const String &p_path) {
	const String ext = p_path.get_extension().to_lower();
	const String name = p_path.get_file().to_lower();
	return ext == "tscn" || ext == "tres" || name == "project.godot" || name == "export_presets.cfg";
}

static String _local_to_res(const String &p_local_path) {
	const String path = p_local_path.replace_char('\\', '/');
	const String root = ProjectSettings::get_singleton()->get_resource_path().replace_char('\\', '/').trim_suffix("/");
#ifdef WINDOWS_ENABLED
	// Perforce reports the workspace root as configured, e.g. "b:\Perforce\..." for
	// a project opened as "B:/Perforce/...", and Windows paths ignore case.
	const bool inside = path.to_lower().begins_with(root.to_lower() + "/");
#else
	const bool inside = path.begins_with(root + "/");
#endif
	return inside ? "res://" + path.substr(root.length() + 1) : path;
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

bool PerforcePlugin::_start_task(const Callable &p_task, const String &p_busy_text, bool p_queue) {
	if (task != WorkerThreadPool::INVALID_TASK_ID) {
		// One thing at a time.
		if (p_queue) {
			queued_tasks.push_back(p_task);
			queued_texts.push_back(p_busy_text);
			return true;
		}
		return false;
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
	if (!queued_tasks.is_empty()) {
		const Callable next = queued_tasks[0];
		const String text = queued_texts[0];
		queued_tasks.remove_at(0);
		queued_texts.remove_at(0);
		_start_task(next, text);
		return; // A pending refresh runs after the queue is empty.
	}
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
	result["server"] = conn.port.is_empty() ? P4Client::get_variable("P4PORT") : conn.port;

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
	const bool offline = status == PerforceDock::STATUS_OFFLINE;
	if (offline || status == PerforceDock::STATUS_LOGIN_REQUIRED) {
		// The server is unreachable: keep working offline and retry in the background.
		// Harmless if the project isn't in Perforce after all: the first successful
		// connection says so, and the remembered changes are dropped.
		// Logged out works the same way: nothing waits on the server, and the
		// remembered changes are sent once logged in.
		_install_hook();
		hook->set_offline(true);
		if (offline) {
			reconnect_timer->start();
		}
	} else if (workspace_name.is_empty()) {
		_remove_hook();
		DirAccess::remove_absolute(_offline_file()); // Not a Perforce project: nothing to send later.
	} else {
		_install_hook(); // Also when logged out: it will ask for a login when it needs one.
		if (connected) {
			hook->set_offline(false);
			reconnect_timer->stop();
			offline_reported = false;
			EditorSettings::get_singleton()->set_project_metadata("perforce", "last_workspace", workspace_name);
			health_checked = false;
			_ensure_p4ignore();
			if (hook->get_offline_work_count() > 0) {
				_start_task(callable_mp(this, &PerforcePlugin::_task_send_offline_work).bind(hook->take_offline_work()), TTR("Sending offline changes to Perforce..."), true);
			}
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
			const String server = p_result.get("server", String());
			if (hook) {
				_update_offline_status();
			} else {
				dock->set_status(status, vformat(TTR("Can't reach the Perforce server %s."), server.is_empty() ? TTR("(no server set)") : server));
			}
			if (offline_reported) {
				break; // Still offline after a retry; already explained.
			}
			offline_reported = true;
			dock->add_log(message, PerforceDock::LOG_ERROR);
			if (hook) {
				dock->add_log(TTR("Working offline: saving still works, and every change is remembered and sent to Perforce automatically when the server is back. Retrying every 30 seconds."), PerforceDock::LOG_WARNING);
			}
			if (message.contains("specified class was not found")) {
				// Winsock error 10109: the p4 program can't use the network at all on this PC.
				dock->add_log(TTR("This is a Windows networking problem on this PC, not a Perforce or Godot one: the p4 command line program can't open network connections (P4V may still work, it doesn't use p4). Fix: open a Command Prompt as administrator, run \"netsh winsock reset\", and restart the PC. If a VPN or security program is installed, check whether it blocks p4.exe."), PerforceDock::LOG_WARNING);
			} else {
				dock->add_log(vformat(TTR("Check the server address with your team (P4V: Connection > Open Connection). To test outside Godot, open a Command Prompt and run: p4 -p %s info"), server.is_empty() ? String("<server:port>") : server), PerforceDock::LOG_WARNING);
			}
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
	result["connection_failed"] = opened.connection_failed;
	result["connection_message"] = opened.get_messages();
	Array files;
	PackedStringArray opened_local;
	for (const P4Result::Record &rec : opened.records) {
		opened_local.push_back(P4Result::field(rec, "clientFile"));
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
	result["opened_local"] = opened_local;

	// What teammates submitted since our last sync, and what still needs merging.
	result["outdated"] = P4Client::run({ "sync", "-n", spec }).records.size();
	result["unresolved"] = P4Client::run({ "resolve", "-n", spec }).records.size();

	// Project setup problems.
	result["cache_files"] = P4Client::run({ "files", "-e", P4Client::escape(project_root) + "/.godot/..." }).records.size();
	result["p4v_ignores"] = !P4Client::get_variable("P4IGNORE").is_empty();
	result["tmp_files"] = P4Client::run({ "files", "-e", P4Client::escape(project_root) + "/....tmp" }).records.size();

	callable_mp(this, &PerforcePlugin::_refresh_done).call_deferred(result);
}

void PerforcePlugin::_refresh_done(const Dictionary &p_result) {
	_finish_task();
	if ((bool)p_result["connection_failed"]) {
		_go_offline(p_result["connection_message"]);
		return;
	}
	if ((bool)p_result["login_required"]) {
		_login_required(String());
		return;
	}
	if (hook) {
		hook->set_known_opened(PackedStringArray(p_result["opened_local"]));
	}
	dock->set_opened_files(p_result["files"]);
	dock->set_remote_state(p_result["outdated"], p_result["unresolved"]);
	dock->set_setup_state(p_result["cache_files"], p_result["p4v_ignores"], p_result["tmp_files"]);
	if (!health_checked) {
		_check_health(); // Once per connection; it reads every file, so not on every refresh.
	}
}

// --- Get latest and resolving -------------------------------------------

void PerforcePlugin::_get_latest() {
	EditorInterface::get_singleton()->save_all_scenes();
	_flush_now();
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
		const String local = ProjectSettings::get_singleton()->globalize_path(path).trim_suffix("/");
		files.push_back(P4Client::escape(local) + (DirAccess::dir_exists_absolute(local) ? "/..." : ""));
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

// --- Revert unchanged ------------------------------------------------------

void PerforcePlugin::_revert_unchanged() {
	_start_task(callable_mp(this, &PerforcePlugin::_task_revert_unchanged), TTR("Reverting unchanged files..."), true);
}

void PerforcePlugin::_task_revert_unchanged() {
	// -a: only files whose content is identical to the depot revision.
	P4Result revert = P4Client::run({ "revert", "-a", _depot_spec() });
	Dictionary result;
	result["count"] = revert.records.size();
	result["errors"] = revert.errors.is_empty() ? String() : revert.get_messages();
	result["login_required"] = revert.login_required;
	callable_mp(this, &PerforcePlugin::_revert_unchanged_done).call_deferred(result);
}

void PerforcePlugin::_revert_unchanged_done(const Dictionary &p_result) {
	_finish_task();
	if ((bool)p_result["login_required"]) {
		_login_required(String());
		return;
	}
	const int count = p_result["count"];
	const String errors = p_result["errors"];
	if (!errors.is_empty()) {
		dock->add_log(TTR("Revert Unchanged:") + " " + errors, PerforceDock::LOG_ERROR);
	} else if (count == 0) {
		dock->add_log(TTR("No unchanged files to revert."), PerforceDock::LOG_INFO);
	} else {
		dock->add_log(vformat(TTRN("Reverted %d unchanged file.", "Reverted %d unchanged files.", count), count), PerforceDock::LOG_SUCCESS);
	}
	EditorFileSystem::get_singleton()->scan_changes();
	_refresh();
}

// --- Workspace chooser -----------------------------------------------------

// Lists the user's workspaces on the server so nobody has to type the name.
void PerforcePlugin::_choose_workspace() {
	const P4Connection saved = P4Client::get_connection();
	P4Connection conn = saved;
	conn.port = port_edit->get_text().strip_edges();
	conn.user = user_edit->get_text().strip_edges();
	conn.client = String();
	P4Client::set_connection(conn);
	const String user = conn.user.is_empty() ? P4Client::get_variable("P4USER") : conn.user;
	P4Result clients = P4Client::run({ "clients", "-u", user });
	P4Client::set_connection(saved);

	if (clients.records.is_empty()) {
		String why = clients.login_required ? TTR("Log in first: enter your password and click Connect, then try again.") : clients.get_messages();
		if (why.is_empty()) {
			why = vformat(TTR("No workspaces found for user \"%s\". Create one in P4V first."), user);
		}
		connection_message->set_text(why);
		connection_message->show();
		return;
	}

	workspace_menu->clear();
	for (const P4Result::Record &client : clients.records) {
		const String name = P4Result::field(client, "client");
		const String root = P4Result::field(client, "Root");
		String label = name + "    " + root;
		if (_is_inside(project_root, root)) {
			label += "    " + TTR("(contains this project)");
		}
		workspace_menu->add_item(label);
		workspace_menu->set_item_metadata(-1, name);
	}
	workspace_menu->reset_size();
	workspace_menu->set_position(client_edit->get_screen_position() + Vector2(0, client_edit->get_size().y));
	workspace_menu->popup();
}

void PerforcePlugin::_workspace_chosen(int p_index) {
	client_edit->set_text(workspace_menu->get_item_metadata(workspace_menu->get_item_index(p_index)));
}

// --- Workspace health ------------------------------------------------------

// Finds files where the workspace and Perforce disagree. Reads every file
// (to compare digests), so it runs once per connection and on request.
void PerforcePlugin::_check_health(bool p_open_review) {
	if (!connected) {
		return;
	}
	open_review_after_check = open_review_after_check || p_open_review;
	if (health_pending) {
		return; // The queued check will open the review if asked.
	}
	health_pending = true;
	dock->set_health(0, 0, 0, true);
	_start_task(callable_mp(this, &PerforcePlugin::_task_health), String(), true);
}

void PerforcePlugin::_task_health() {
	const String spec = _depot_spec();
	auto locals = [](const P4Result &p_result, bool p_skip_tmp) {
		PackedStringArray paths;
		for (const P4Result::Record &rec : p_result.records) {
			const String path = P4Result::field(rec, "clientFile").replace_char('\\', '/');
			if (!path.is_empty() && !(p_skip_tmp && path.ends_with(".tmp"))) {
				paths.push_back(path);
			}
		}
		paths.sort();
		return paths;
	};
	Dictionary result;
	P4Result changed = P4Client::run({ "diff", "-se", spec }); // Unopened files that differ from the depot.
	P4Result added = P4Client::run({ "reconcile", "-n", "-a", spec }); // Files not in the depot (honors .p4ignore).
	P4Result missing = P4Client::run({ "diff", "-sd", spec }); // Unopened files missing on disk.
	result["changed"] = locals(changed, false);
	result["new"] = locals(added, true);
	result["missing"] = locals(missing, true); // .tmp junk has its own cleanup button.
	result["connection_failed"] = changed.connection_failed || added.connection_failed || missing.connection_failed;
	callable_mp(this, &PerforcePlugin::_health_done).call_deferred(result);
}

void PerforcePlugin::_health_done(const Dictionary &p_result) {
	health_pending = false;
	_finish_task();
	if ((bool)p_result["connection_failed"]) {
		_go_offline(String());
		return;
	}
	health_changed = p_result["changed"];
	health_new = p_result["new"];
	health_missing = p_result["missing"];
	health_checked = true;
	dock->set_health(health_changed.size(), health_new.size(), health_missing.size(), false);
	const String summary = vformat("%d/%d/%d", health_changed.size(), health_new.size(), health_missing.size());
	if (summary != health_logged && (!health_changed.is_empty() || !health_new.is_empty() || !health_missing.is_empty())) {
		health_logged = summary;
		dock->add_log(vformat(TTR("Workspace check: %d changed but not checked out, %d new files not in Perforce, %d missing on disk. Click Review to sort them out."), health_changed.size(), health_new.size(), health_missing.size()), PerforceDock::LOG_WARNING);
	}
	if (open_review_after_check) {
		open_review_after_check = false;
		review_dialog->open(project_root, health_changed, health_new, health_missing);
	}
}

void PerforcePlugin::_review() {
	// Always look again first: files may have changed since the last check.
	_check_health(true);
}

void PerforcePlugin::_review_apply(const String &p_action, const PackedStringArray &p_paths) {
	if (p_action == "delete") {
		_confirm(TTR("Mark for Delete"), vformat(TTRN("Mark %d file for delete in Perforce? When you submit, it's deleted for the whole team.", "Mark %d files for delete in Perforce? When you submit, they're deleted for the whole team.", p_paths.size()), p_paths.size()), TTR("Mark for Delete"), callable_mp(this, &PerforcePlugin::_task_review_apply).bind(p_action, p_paths));
		return;
	}
	_start_task(callable_mp(this, &PerforcePlugin::_task_review_apply).bind(p_action, p_paths), TTR("Updating Perforce..."), true);
}

void PerforcePlugin::_task_review_apply(const String &p_action, const PackedStringArray &p_paths) {
	Vector<String> files;
	for (const String &path : p_paths) {
		if (p_action == "add") {
			files.push_back(path); // `add -f` takes unescaped names.
		} else if (p_action == "restore") {
			files.push_back(P4Client::escape(path) + "#have"); // Bring back the revision we had.
		} else {
			files.push_back(P4Client::escape(path));
		}
	}
	P4Result result;
	if (p_action == "checkout") {
		result = P4Client::run_files({ "edit" }, files);
	} else if (p_action == "add") {
		result = P4Client::run_files({ "add", "-f" }, files);
	} else if (p_action == "restore") {
		result = P4Client::run_files({ "sync", "-f" }, files);
	} else if (p_action == "delete") {
		result = P4Client::run_files({ "delete" }, files);
	}
	Dictionary out;
	out["action"] = p_action;
	out["count"] = result.records.size();
	out["requested"] = p_paths.size();
	out["messages"] = result.errors.is_empty() ? String() : result.get_messages();
	out["login_required"] = result.login_required;
	callable_mp(this, &PerforcePlugin::_review_apply_done).call_deferred(out);
}

void PerforcePlugin::_review_apply_done(const Dictionary &p_result) {
	_finish_task();
	if ((bool)p_result["login_required"]) {
		_login_required(String());
		return;
	}
	const String action = p_result["action"];
	const int count = p_result["count"];
	String message;
	if (action == "checkout") {
		message = vformat(TTRN("Checked out %d changed file.", "Checked out %d changed files.", count), count);
	} else if (action == "add") {
		message = vformat(TTRN("Marked %d new file for add.", "Marked %d new files for add.", count), count);
	} else if (action == "restore") {
		message = vformat(TTRN("Restored %d missing file from Perforce.", "Restored %d missing files from Perforce.", count), count);
		EditorFileSystem::get_singleton()->scan_changes();
	} else {
		message = vformat(TTRN("Marked %d missing file for delete. Submit to delete it for everyone, or revert to keep it.", "Marked %d missing files for delete. Submit to delete them for everyone, or revert to keep them.", count), count);
	}
	dock->add_log(message, PerforceDock::LOG_SUCCESS);
	const String messages = p_result["messages"];
	if (!messages.is_empty()) {
		dock->add_log(messages, PerforceDock::LOG_WARNING);
	}
	_check_health();
	_refresh();
}

// --- Leftover .tmp files ----------------------------------------------------

void PerforcePlugin::_remove_tmp() {
	_confirm(TTR("Remove Leftover .tmp Files"),
			TTR("Godot leaves name1234.tmp files behind when a save is interrupted. They're junk.\n\nThis submits a change that deletes the ones in Perforce, and deletes leftover ones on your disk (only files named like an existing file plus digits and .tmp). .p4ignore already keeps new ones out."),
			TTR("Remove and Submit"), callable_mp(this, &PerforcePlugin::_task_remove_tmp));
}

void PerforcePlugin::_task_remove_tmp() {
	Dictionary result;
	const String spec = P4Client::escape(project_root) + "/....tmp";
	P4Result del = P4Client::run({ "delete", spec });
	String change;
	if (!del.records.is_empty()) {
		P4Result submit = P4Client::run({ "submit", "-d", "Remove leftover .tmp files from interrupted Godot saves.", spec });
		for (const P4Result::Record &rec : submit.records) {
			if (rec.has("submittedChange")) {
				change = P4Result::field(rec, "submittedChange");
			}
		}
		result["messages"] = submit.errors.is_empty() ? String() : submit.get_messages();
	}
	result["count"] = del.records.size();
	result["change"] = change;
	result["login_required"] = del.login_required;
	callable_mp(this, &PerforcePlugin::_remove_tmp_done).call_deferred(result);
}

// Deletes Godot's safe-save leftovers: "<existing file><digits>.tmp".
int PerforcePlugin::_remove_local_tmp_leftovers(const String &p_dir) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return 0;
	}
	int removed = 0;
	for (const String &file : da->get_files()) {
		if (!file.ends_with(".tmp")) {
			continue;
		}
		String original = file.trim_suffix(".tmp");
		while (!original.is_empty() && is_digit(original[original.length() - 1])) {
			original = original.left(-1);
		}
		if (original.length() < file.length() - 4 && FileAccess::exists(p_dir.path_join(original))) {
			const String path = p_dir.path_join(file);
			if (P4Client::is_read_only(path)) {
				P4Client::make_writable(path);
			}
			if (DirAccess::remove_absolute(path) == OK) {
				removed++;
			}
		}
	}
	for (const String &dir : da->get_directories()) {
		if (dir != ".godot") {
			removed += _remove_local_tmp_leftovers(p_dir.path_join(dir));
		}
	}
	return removed;
}

void PerforcePlugin::_remove_tmp_done(const Dictionary &p_result) {
	_finish_task();
	if ((bool)p_result["login_required"]) {
		_login_required(String());
		return;
	}
	const int local = _remove_local_tmp_leftovers(project_root);
	const String change = p_result["change"];
	const int count = p_result["count"];
	if (count > 0 && change.is_empty()) {
		dock->add_log(TTR("Removing .tmp files from Perforce failed:") + " " + String(p_result.get("messages", String())), PerforceDock::LOG_ERROR);
	} else {
		dock->add_log(vformat(TTR("Removed %d leftover .tmp files from Perforce%s and %d from disk."), count, change.is_empty() ? String() : vformat(" (change %s)", change), local), PerforceDock::LOG_SUCCESS);
	}
	EditorFileSystem::get_singleton()->scan_changes();
	_refresh();
}

// --- Keeping the .godot cache out of Perforce ------------------------------

// Every Perforce project needs a .p4ignore listing .godot/; create or fix it.
// New files are marked for add by the hook like any other file.
void PerforcePlugin::_ensure_p4ignore() {
	const String path = project_root.path_join(".p4ignore");
	String text;
	bool has_cache = false, has_tmp = false;
	if (FileAccess::exists(path)) {
		text = FileAccess::get_file_as_string(path);
		for (const String &line : text.replace("\r", "").split("\n")) {
			const String entry = line.strip_edges();
			has_cache = has_cache || entry == ".godot" || entry == ".godot/" || entry == "/.godot/" || entry == ".godot/*" || entry == ".godot/**";
			has_tmp = has_tmp || entry == "*.tmp";
		}
		if (has_cache && has_tmp) {
			return; // Already complete.
		}
		if (!text.is_empty() && !text.ends_with("\n")) {
			text += "\n";
		}
	} else {
		text = "# Files Perforce should never add. Godot's .godot folder is a local cache.\n";
	}
	if (!has_cache) {
		text += ".godot/\n";
	}
	if (!has_tmp) {
		text += "# Leftovers from interrupted saves (name1234.tmp).\n*.tmp\n";
	}

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		dock->add_log(TTR("Couldn't write .p4ignore."), PerforceDock::LOG_WARNING);
		return;
	}
	file->store_string(text);
	file.unref();
	dock->add_log(TTR("Updated .p4ignore so Perforce never adds the .godot cache or leftover .tmp files. Submit .p4ignore so your team gets it."), PerforceDock::LOG_SUCCESS);
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
		if (P4Client::is_read_only(path)) {
			P4Client::make_writable(path);
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
		_flush_now(); // New files must be marked for add before submitting.
	}
	_start_task(confirm_action, TTR("Talking to the server..."), true);
	confirm_action = Callable();
}

// --- Hook ----------------------------------------------------------------

void PerforcePlugin::_install_hook() {
	if (hook) {
		return;
	}
	hook = memnew(PerforceWriteHook(project_root, callable_mp(this, &PerforcePlugin::_login_required), callable_mp(this, &PerforcePlugin::_activity), callable_mp(this, &PerforcePlugin::_go_offline)));
	// Until a connection check succeeds, don't let saves wait for the server:
	// remember changes and hand them to Perforce once connected.
	hook->set_offline(true);
	_load_offline_work();
	FileSystemWriteHook::set_singleton(hook);
	_apply_auto_checkout();
	if (flush_timer->is_inside_tree()) {
		flush_timer->start();
	}
}

void PerforcePlugin::_remove_hook() {
	if (!hook) {
		return;
	}
	flush_timer->stop();
	_flush_now();
	_save_offline_work(); // Work done offline survives an editor restart.
	if (FileSystemWriteHook::get_singleton() == hook) {
		FileSystemWriteHook::set_singleton(nullptr);
	}
	memdelete(hook);
	hook = nullptr;
}

void PerforcePlugin::_activity(const String &p_message, int p_level) {
	dock->add_log(p_message, (PerforceDock::LogLevel)p_level);
	if (p_level == PerforceDock::LOG_ERROR) {
		EditorToaster::get_singleton()->popup_str("Perforce: " + p_message, EditorToaster::SEVERITY_WARNING);
	}
	refresh_timer->start(); // Update the file list once things settle down.
}

// Runs queued `p4 add` / `p4 edit` commands on a worker thread, so saving never waits for the server.
void PerforcePlugin::_flush_pending() {
	if (hook && hook->take_offline_dirty()) {
		_save_offline_work();
		_update_offline_status();
	}
	if (!hook || !hook->has_pending()) {
		return;
	}
	if (flush_task != WorkerThreadPool::INVALID_TASK_ID) {
		if (!WorkerThreadPool::get_singleton()->is_task_completed(flush_task)) {
			return; // Still busy; the next tick picks up the rest.
		}
		_wait_for_flush();
	}
	flush_task = WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &PerforcePlugin::_task_flush), false, "Perforce flush");
}

void PerforcePlugin::_task_flush() {
	hook->flush_pending();
}

void PerforcePlugin::_wait_for_flush() {
	if (flush_task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(flush_task);
		flush_task = WorkerThreadPool::INVALID_TASK_ID;
	}
}

// Flushes everything before an operation that depends on it (submit, get latest, shutdown).
void PerforcePlugin::_flush_now() {
	_wait_for_flush();
	if (hook) {
		hook->flush_pending();
	}
}

bool PerforcePlugin::_is_auto_checkout() const {
	return EditorSettings::get_singleton()->get_project_metadata("perforce", "auto_checkout", true);
}

bool PerforcePlugin::_is_auto_delete() const {
	// Off by default: deleting in Godot never marks files for delete on its own.
	return EditorSettings::get_singleton()->get_project_metadata("perforce", "auto_delete", false);
}

void PerforcePlugin::_apply_auto_checkout() {
	if (hook) {
		hook->set_auto_checkout(_is_auto_checkout());
		hook->set_auto_delete(_is_auto_delete());
	}
}

// --- Offline mode ----------------------------------------------------------

// A command found the server unreachable in the middle of a session.
void PerforcePlugin::_go_offline(const String &p_message) {
	if (!hook) {
		return;
	}
	hook->set_offline(true);
	const bool was_connected = connected;
	connected = false;
	reconnect_timer->start();
	_update_offline_status();
	if (was_connected || !offline_reported) {
		offline_reported = true;
		dock->add_log(TTR("Lost the connection to the Perforce server.") + (p_message.is_empty() ? String() : " " + p_message), PerforceDock::LOG_ERROR);
		dock->add_log(TTR("Working offline: saving still works, and every change is remembered and sent to Perforce automatically when the server is back. Retrying every 30 seconds."), PerforceDock::LOG_WARNING);
		EditorToaster::get_singleton()->popup_str(TTR("Perforce: server unreachable, working offline. Changes will be sent when it's back."), EditorToaster::SEVERITY_WARNING);
	}
}

void PerforcePlugin::_reconnect() {
	if (connected || task != WorkerThreadPool::INVALID_TASK_ID) {
		return;
	}
	_apply_connection_settings();
	_start_task(callable_mp(this, &PerforcePlugin::_task_connect), String());
}

void PerforcePlugin::_update_offline_status() {
	if (!hook || connected || dock->get_status() == PerforceDock::STATUS_LOGIN_REQUIRED) {
		return;
	}
	const int count = hook->get_offline_work_count();
	dock->set_status(PerforceDock::STATUS_OFFLINE, count == 0 ? TTR("Offline: can't reach the Perforce server. Saving still works; changes are sent when it's back.") : vformat(TTRN("Offline: %d change waiting to be sent to Perforce when the server is back.", "Offline: %d changes waiting to be sent to Perforce when the server is back.", count), count));
}

String PerforcePlugin::_offline_file() const {
	return project_root.path_join(".godot/editor/perforce_offline.json");
}

void PerforcePlugin::_save_offline_work() {
	if (!hook) {
		return;
	}
	if (hook->get_offline_work_count() == 0) {
		if (FileAccess::exists(_offline_file())) {
			DirAccess::remove_absolute(_offline_file());
		}
		return;
	}
	Ref<FileAccess> file = FileAccess::open(_offline_file(), FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string(JSON::stringify(hook->get_offline_work(), "\t"));
	}
}

void PerforcePlugin::_load_offline_work() {
	if (!hook || !FileAccess::exists(_offline_file())) {
		return;
	}
	const Variant work = JSON::parse_string(FileAccess::get_file_as_string(_offline_file()));
	if (work.get_type() == Variant::DICTIONARY) {
		hook->add_offline_work(work);
	}
}

// Hands everything done offline to Perforce: moves first (keeping history), then
// `p4 reconcile` opens edited/new/deleted files (only if they really changed).
void PerforcePlugin::_task_send_offline_work(const Dictionary &p_work) {
	Dictionary result;
	Dictionary unsent;
	PackedStringArray unsent_files;
	PackedStringArray unsent_moves;
	int moved = 0;
	bool went_offline = false;

	const PackedStringArray moves = p_work.get("moves", PackedStringArray());
	PackedStringArray files = p_work.get("files", PackedStringArray());
	for (int i = 0; i + 1 < moves.size(); i += 2) {
		const String from = moves[i], to = moves[i + 1];
		const bool folder = DirAccess::dir_exists_absolute(to);
		const String from_spec = P4Client::escape(from) + (folder ? "/..." : "");
		const String to_spec = P4Client::escape(to) + (folder ? "/..." : "");
		if (went_offline) {
			unsent_moves.push_back(from);
			unsent_moves.push_back(to);
			continue;
		}
		// -k: the files were already moved on disk; only tell the server.
		P4Result edit = P4Client::run({ "edit", "-k", from_spec });
		P4Result move = P4Client::run({ "move", "-k", from_spec, to_spec });
		if (edit.connection_failed || move.connection_failed) {
			went_offline = true;
			unsent_moves.push_back(from);
			unsent_moves.push_back(to);
		} else if (!move.records.is_empty()) {
			moved += move.records.size();
		} else {
			// Not versioned (or couldn't move): the new location is new; the old one is
			// only deleted in Perforce when automatic delete is on.
			if (hook && hook->is_auto_delete()) {
				files.push_back(from);
			}
			files.push_back(to);
		}
	}

	Vector<String> specs;
	for (const String &file : files) {
		specs.push_back(P4Client::escape(file) + (DirAccess::dir_exists_absolute(file) ? "/..." : ""));
	}
	int edited = 0, added = 0, deleted = 0;
	if (!went_offline && !specs.is_empty()) {
		// -d (mark missing files for delete) only with automatic delete on; otherwise
		// they show up as "missing on disk" for the user to decide.
		const bool deletes = hook && hook->is_auto_delete();
		P4Result reconcile = deletes ? P4Client::run_files({ "reconcile", "-e", "-a", "-d" }, specs) : P4Client::run_files({ "reconcile", "-e", "-a" }, specs);
		if (reconcile.connection_failed) {
			went_offline = true;
		}
		for (const P4Result::Record &rec : reconcile.records) {
			const String action = P4Result::field(rec, "action");
			if (action == "edit") {
				edited++;
			} else if (action == "add") {
				added++;
			} else if (action == "delete") {
				deleted++;
			}
		}
		// Files someone else has locked were changed here but can't be checked out: say so.
		PackedStringArray locked;
		for (const String &warning : reconcile.warnings) {
			if (warning.contains("can't edit exclusive")) {
				locked.push_back(warning.get_slicec(' ', 0).get_file());
			}
		}
		Vector<String> holders;
		for (const String &info : reconcile.infos) {
			const int at = info.find("also opened by ");
			if (at != -1 && !holders.has(info.substr(at + 15).get_slicec('@', 0))) {
				holders.push_back(info.substr(at + 15).get_slicec('@', 0));
			}
		}
		result["locked"] = locked;
		result["holders"] = String(", ").join(holders);
		result["messages"] = reconcile.errors.is_empty() ? String() : reconcile.get_messages();
	}
	if (went_offline) {
		unsent_files = PackedStringArray(files);
	}
	unsent["files"] = unsent_files;
	unsent["moves"] = unsent_moves;

	result["unsent"] = unsent;
	result["went_offline"] = went_offline;
	result["edited"] = edited;
	result["added"] = added;
	result["deleted"] = deleted;
	result["moved"] = moved;
	callable_mp(this, &PerforcePlugin::_send_offline_work_done).call_deferred(result);
}

void PerforcePlugin::_send_offline_work_done(const Dictionary &p_result) {
	_finish_task();
	if (hook) {
		hook->add_offline_work(p_result["unsent"]); // Whatever didn't make it, for the next try.
	}
	if ((bool)p_result["went_offline"]) {
		_go_offline(String());
		_save_offline_work();
		return;
	}
	const int edited = p_result["edited"], added = p_result["added"], deleted = p_result["deleted"], moved = p_result["moved"];
	dock->add_log(vformat(TTR("Sent offline changes to Perforce: %d checked out, %d added, %d deleted, %d moved (files that didn't really change were left alone)."), edited, added, deleted, moved), PerforceDock::LOG_SUCCESS);
	const String messages = p_result.get("messages", String());
	if (!messages.is_empty()) {
		dock->add_log(messages, PerforceDock::LOG_WARNING);
	}
	const PackedStringArray locked = p_result.get("locked", PackedStringArray());
	if (!locked.is_empty()) {
		const String message = vformat(TTR("You changed %s while offline, but it's locked by %s, so it couldn't be checked out. Your changes are still on disk; ask them to submit or revert it, then check it out (right-click > Perforce: Check Out)."), String(", ").join(locked), String(p_result.get("holders", String())));
		dock->add_log(message, PerforceDock::LOG_ERROR);
		EditorToaster::get_singleton()->popup_str("Perforce: " + message, EditorToaster::SEVERITY_WARNING);
	}
	_save_offline_work();
	_refresh();
}

// --- Right-click menus ---------------------------------------------------

void PerforcePlugin::context_action(const Variant &p_selection, int p_action) {
	PackedStringArray paths;
	if (p_selection.get_type() == Variant::ARRAY) {
		// Scene tree: the open scene's file, plus instanced scenes that were selected.
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		if (root && !root->get_scene_file_path().is_empty()) {
			paths.push_back(root->get_scene_file_path());
		}
		const Array nodes = p_selection;
		for (const Variant &v : nodes) {
			Node *node = Object::cast_to<Node>(v.get_validated_object());
			if (node && node != root && !node->get_scene_file_path().is_empty() && !paths.has(node->get_scene_file_path())) {
				paths.push_back(node->get_scene_file_path());
			}
		}
	} else {
		paths = p_selection;
	}
	if (paths.is_empty()) {
		dock->add_log(TTR("Save the scene first, then check it out."), PerforceDock::LOG_WARNING);
		return;
	}

	if (p_action == PerforceContextMenu::ACTION_REVERT) {
		_revert(paths);
		return;
	}
	const String busy = p_action == PerforceContextMenu::ACTION_ADD ? TTR("Marking files for add...") : TTR("Checking out...");
	_start_task(callable_mp(this, &PerforcePlugin::_task_files).bind(p_action, paths), busy, true);
}

void PerforcePlugin::_task_files(int p_action, const PackedStringArray &p_paths) {
	Vector<String> files, folders;
	for (const String &path : p_paths) {
		const String local = ProjectSettings::get_singleton()->globalize_path(path).trim_suffix("/");
		if (DirAccess::dir_exists_absolute(local)) {
			folders.push_back(P4Client::escape(local) + "/...");
		} else {
			files.push_back(local);
		}
	}

	P4Result result;
	result.exit_code = 0;
	auto merge = [&result](const P4Result &p_part) {
		for (const P4Result::Record &record : p_part.records) {
			result.records.push_back(P4Result::Record(record));
		}
		result.infos.append_array(p_part.infos);
		result.warnings.append_array(p_part.warnings);
		result.errors.append_array(p_part.errors);
		result.login_required = result.login_required || p_part.login_required;
	};

	if (p_action == PerforceContextMenu::ACTION_ADD) {
		if (!files.is_empty()) {
			merge(P4Client::run_files({ "add", "-f" }, files)); // -f: unescaped names with @ # % *.
		}
		for (const String &folder : folders) {
			merge(P4Client::run({ "reconcile", "-a", folder })); // New files in the folder, honoring .p4ignore.
		}
	} else {
		Vector<String> specs = folders;
		for (const String &file : files) {
			specs.push_back(P4Client::escape(file));
		}
		merge(P4Client::run_files({ "edit" }, specs));
		// `p4 edit` makes the files writable; also catch ones it reported as already open.
	}

	// Tagged output: one record per file opened; "currently opened" for files that already were.
	int done = result.records.size();
	int already = 0;
	for (const String &info : result.infos) {
		if (info.contains("currently opened")) {
			already++;
		}
	}
	Dictionary out;
	out["action"] = p_action;
	out["done"] = done;
	out["already"] = already;
	out["requested"] = p_paths.size();
	out["messages"] = result.get_messages();
	out["login_required"] = result.login_required;
	callable_mp(this, &PerforcePlugin::_files_done).call_deferred(out);
}

void PerforcePlugin::_files_done(const Dictionary &p_result) {
	_finish_task();
	if ((bool)p_result["login_required"]) {
		_login_required(String());
		return;
	}
	const bool add = (int)p_result["action"] == PerforceContextMenu::ACTION_ADD;
	const String messages = p_result["messages"];
	const int done = p_result["done"];
	const int already = p_result["already"];
	if (already > 0) {
		dock->add_log(vformat(TTRN("%d file was already checked out.", "%d files were already checked out.", already), already), PerforceDock::LOG_INFO);
	}
	if (done > 0) {
		dock->add_log(add ? vformat(TTRN("Marked %d file for add.", "Marked %d files for add.", done), done) : vformat(TTRN("Checked out %d file.", "Checked out %d files.", done), done), PerforceDock::LOG_SUCCESS);
	}
	if (!messages.is_empty()) {
		// Not in the depot, locked, ignored... Perforce explains it best.
		dock->add_log(messages, done > 0 ? PerforceDock::LOG_INFO : PerforceDock::LOG_WARNING);
	}
	_refresh();
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
	String client_hint = workspace_name.is_empty() ? P4Client::get_variable("P4CLIENT") : workspace_name;
	client_edit->set_placeholder(client_hint.is_empty() ? TTR("Detected automatically, or click Choose...") : client_hint);
	password_edit->clear();
	auto_checkout_check->set_pressed(_is_auto_checkout());
	auto_delete_check->set_pressed(_is_auto_delete());

	connection_message->set_text(p_message);
	connection_message->set_visible(!p_message.is_empty());
	connection_dialog->popup_centered(Size2(500, 0) * EDSCALE);
	password_edit->grab_focus();
}

void PerforcePlugin::_connection_dialog_confirmed() {
	const String workspace = client_edit->get_text().strip_edges();
	if (workspace.contains(":") || workspace.contains("\\") || workspace.contains("/")) {
		// A common mistake: the workspace *folder* instead of its name.
		callable_mp(this, &PerforcePlugin::_show_connection_dialog).call_deferred(vformat(TTR("\"%s\" looks like a folder. Workspace needs the workspace *name* (as shown in P4V's workspace dropdown, e.g. name_PC). Click \"Choose...\" to pick it, or leave it empty to detect it automatically."), workspace));
		return;
	}
	EditorSettings::get_singleton()->set_project_metadata("perforce", "port", port_edit->get_text().strip_edges());
	EditorSettings::get_singleton()->set_project_metadata("perforce", "user", user_edit->get_text().strip_edges());
	EditorSettings::get_singleton()->set_project_metadata("perforce", "client", client_edit->get_text().strip_edges());
	if (auto_checkout_check->is_pressed() != _is_auto_checkout()) {
		EditorSettings::get_singleton()->set_project_metadata("perforce", "auto_checkout", auto_checkout_check->is_pressed());
		dock->add_log(auto_checkout_check->is_pressed() ? TTR("Automatic checkout is on: saving checks files out and marks new files for add.") : TTR("Automatic checkout is off: check files out with right-click > Perforce: Check Out before editing them."), PerforceDock::LOG_INFO);
	}
	if (auto_delete_check->is_pressed() != _is_auto_delete()) {
		EditorSettings::get_singleton()->set_project_metadata("perforce", "auto_delete", auto_delete_check->is_pressed());
		dock->add_log(auto_delete_check->is_pressed() ? TTR("Automatic delete is on: deleting a file in Godot marks it for delete in Perforce.") : TTR("Automatic delete is off: deleting a file in Godot only deletes it on disk; review it under \"Missing on disk\" in the Perforce panel."), PerforceDock::LOG_INFO);
	}
	_apply_auto_checkout();
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

void PerforcePlugin::_bind_methods() {
	// Same entry point as the right-click menus; also usable from editor scripts.
	ClassDB::bind_method(D_METHOD("context_action", "selection", "action"), &PerforcePlugin::context_action);
}


void PerforcePlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			project_root = ProjectSettings::get_singleton()->get_resource_path().replace_char('\\', '/').simplify_path();
			add_dock(dock);
			EditorInterface::get_singleton()->get_base_control()->add_child(connection_dialog);
			EditorInterface::get_singleton()->get_base_control()->add_child(confirm_dialog);
			EditorInterface::get_singleton()->get_base_control()->add_child(resolve_dialog);
			EditorInterface::get_singleton()->get_base_control()->add_child(review_dialog);
			add_child(flush_timer);
			add_child(refresh_timer);
			add_child(poll_timer);
			add_child(reconnect_timer);
			filesystem_menu = Ref<PerforceContextMenu>(memnew(PerforceContextMenu(get_instance_id(), true)));
			scene_tabs_menu = Ref<PerforceContextMenu>(memnew(PerforceContextMenu(get_instance_id(), true)));
			scene_tree_menu = Ref<PerforceContextMenu>(memnew(PerforceContextMenu(get_instance_id(), false)));
			add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM, filesystem_menu);
			add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_SCENE_TABS, scene_tabs_menu);
			add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_SCENE_TREE, scene_tree_menu);
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
			_flush_now();
			remove_context_menu_plugin(filesystem_menu);
			remove_context_menu_plugin(scene_tabs_menu);
			remove_context_menu_plugin(scene_tree_menu);
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
	dock->connect("remove_tmp_requested", callable_mp(this, &PerforcePlugin::_remove_tmp));
	dock->connect("review_requested", callable_mp(this, &PerforcePlugin::_review));
	dock->connect("health_check_requested", callable_mp(this, &PerforcePlugin::_check_health).bind(false));
	dock->connect("revert_unchanged_requested", callable_mp(this, &PerforcePlugin::_revert_unchanged));
	dock->connect("enable_p4ignore_requested", callable_mp(this, &PerforcePlugin::_enable_p4ignore));

	flush_timer = memnew(Timer);
	flush_timer->set_wait_time(0.5);
	flush_timer->connect("timeout", callable_mp(this, &PerforcePlugin::_flush_pending));

	refresh_timer = memnew(Timer);
	refresh_timer->set_wait_time(1.5);
	refresh_timer->set_one_shot(true);
	refresh_timer->connect("timeout", callable_mp(this, &PerforcePlugin::_refresh));

	reconnect_timer = memnew(Timer);
	reconnect_timer->set_wait_time(30);
	reconnect_timer->connect("timeout", callable_mp(this, &PerforcePlugin::_reconnect));

	poll_timer = memnew(Timer);
	poll_timer->set_wait_time(120);
	poll_timer->connect("timeout", callable_mp(this, &PerforcePlugin::_refresh));

	confirm_dialog = memnew(ConfirmationDialog);
	confirm_dialog->connect(SceneStringName(confirmed), callable_mp(this, &PerforcePlugin::_confirmed));

	review_dialog = memnew(PerforceReviewDialog);
	review_dialog->connect("apply_requested", callable_mp(this, &PerforcePlugin::_review_apply));

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
	{
		Label *label = memnew(Label(TTR("Workspace:")));
		grid->add_child(label);
		HBoxContainer *row = memnew(HBoxContainer);
		row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		grid->add_child(row);
		client_edit = memnew(LineEdit);
		client_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		row->add_child(client_edit);
		Button *choose = memnew(Button(TTR("Choose...")));
		choose->set_tooltip_text(TTR("List your workspaces on the server."));
		choose->connect(SceneStringName(pressed), callable_mp(this, &PerforcePlugin::_choose_workspace));
		row->add_child(choose);
		workspace_menu = memnew(PopupMenu);
		workspace_menu->connect(SceneStringName(id_pressed), callable_mp(this, &PerforcePlugin::_workspace_chosen));
		row->add_child(workspace_menu);
	}
	password_edit = add_field(TTR("Password:"));
	password_edit->set_secret(true);
	password_edit->set_placeholder(TTR("Only needed when not logged in"));
	connection_dialog->register_text_enter(password_edit);

	auto_checkout_check = memnew(CheckBox);
	auto_checkout_check->set_text(TTR("Check out and add files automatically when saving"));
	auto_checkout_check->set_tooltip_text(TTR("When off, saving a file that isn't checked out fails with a message; use right-click > Perforce: Check Out first."));
	vbox->add_child(auto_checkout_check);

	auto_delete_check = memnew(CheckBox);
	auto_delete_check->set_text(TTR("Mark files for delete in Perforce when deleted in Godot"));
	auto_delete_check->set_tooltip_text(TTR("Off (recommended): deleting a file in Godot only deletes it on disk. It shows up under \"Missing on disk\" in the Perforce panel, where you can restore it or deliberately mark it for delete."));
	vbox->add_child(auto_delete_check);

	Label *hint = memnew(Label(TTR("Leave fields empty to use the same settings as P4V. Your password is sent to \"p4 login\" and never stored.")));
	hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	hint->set_modulate(Color(1, 1, 1, 0.65));
	vbox->add_child(hint);

	for (Node *node : { (Node *)dock, (Node *)connection_dialog, (Node *)confirm_dialog, (Node *)resolve_dialog, (Node *)review_dialog, (Node *)flush_timer, (Node *)refresh_timer, (Node *)poll_timer, (Node *)reconnect_timer }) {
		owned_nodes.push_back(node->get_instance_id());
	}

	// Install the hook now: the editor writes its caches (and maybe project.godot)
	// while loading, before plugins enter the tree or the background connection
	// check finishes. It's removed again if the project isn't in a Perforce workspace.
	_apply_connection_settings();
	_install_hook();
}

PerforcePlugin::~PerforcePlugin() {
	_remove_hook();
	// The dialogs live under the editor's base control, which may already be
	// freed when plugins are destroyed: only touch nodes that still exist, and
	// only free the ones nothing else owns (e.g. the dock after remove_dock()).
	for (ObjectID id : owned_nodes) {
		Node *node = ObjectDB::get_instance<Node>(id);
		if (node && !node->get_parent()) {
			memdelete(node);
		}
	}
}
