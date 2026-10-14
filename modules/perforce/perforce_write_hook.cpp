/**************************************************************************/
/*  perforce_write_hook.cpp                                               */
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

#include "perforce_write_hook.h"

#include "perforce_dock.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"

thread_local bool PerforceWriteHook::suppressed = false;

// Number of flushes a queued add waits for its file to appear (written on another thread).
static const int ADD_ATTEMPTS = 20;

static bool _is_deleted_action(const String &p_action) {
	return p_action == "delete" || p_action == "move/delete" || p_action == "purge" || p_action == "archive";
}

static String _join_names(const Vector<String> &p_names, const String &p_many) {
	if (p_names.size() <= 3) {
		return String(", ").join(p_names);
	}
	return vformat(p_many, p_names.size());
}

String PerforceWriteHook::_normalize(const String &p_path) const {
	String path = p_path;
	if (path.begins_with("res://")) {
		path = ProjectSettings::get_singleton()->globalize_path(path);
	}
	path = path.trim_prefix(R"(\\?\)").replace_char('\\', '/');
	return path.simplify_path();
}

bool PerforceWriteHook::_is_managed(const String &p_abs_path) const {
#ifdef WINDOWS_ENABLED
	const String path = p_abs_path.to_lower();
#else
	const String &path = p_abs_path;
#endif
	if (!path.begins_with(project_root) || path.begins_with(ignored_root)) {
		return false;
	}
	// Editor temporaries and backup files.
	return !path.ends_with(".tmp") && !path.ends_with("~");
}

// The .godot folder is the editor's local cache and never belongs in Perforce.
// If it was submitted by mistake its files are read-only and every editor
// write fails (slowly); just make them writable again.
bool PerforceWriteHook::_unlock_cache_file(const String &p_abs_path) const {
#ifdef WINDOWS_ENABLED
	if (!p_abs_path.to_lower().begins_with(ignored_root)) {
#else
	if (!p_abs_path.begins_with(ignored_root)) {
#endif
		return false;
	}
	if (FileAccess::exists(p_abs_path) && P4Client::is_read_only(p_abs_path)) {
		P4Client::make_writable(p_abs_path);
	}
	return true;
}

// Case-insensitive on Windows, like its file system.
String PerforceWriteHook::_key(const String &p_abs_path) const {
#ifdef WINDOWS_ENABLED
	return p_abs_path.to_lower();
#else
	return p_abs_path;
#endif
}

// .import and .uid files are rewritten by the editor on its own (imports,
// UID assignment). They must never block on Perforce or fail to save.
bool PerforceWriteHook::_is_metadata(const String &p_abs_path) const {
	return p_abs_path.ends_with(".import") || p_abs_path.ends_with(".uid");
}

void PerforceWriteHook::_queue_edit(const String &p_abs_path) {
	MutexLock lock(pending_mutex);
	const String key = _key(p_abs_path);
	if (!known_opened.has(key) && !known_untracked.has(key)) {
		pending_edits.insert(p_abs_path);
	}
}

void PerforceWriteHook::_record_offline(const String &p_abs_path) {
	MutexLock lock(pending_mutex);
	pending_adds.erase(p_abs_path);
	pending_edits.erase(p_abs_path);
	offline_changes.insert(p_abs_path);
	offline_dirty = true;
}

// A command couldn't reach the server: switch to offline mode (once) and tell the plugin.
bool PerforceWriteHook::_went_offline(const P4Result &p_result) {
	if (!p_result.connection_failed) {
		return false;
	}
	if (!offline.is_set()) {
		offline.set();
		on_offline.call_deferred(p_result.get_messages());
	}
	return true;
}

void PerforceWriteHook::set_offline(bool p_offline) {
	offline.set_to(p_offline);
}

Dictionary PerforceWriteHook::get_offline_work() {
	MutexLock lock(pending_mutex);
	PackedStringArray files;
	for (const String &path : offline_changes) {
		files.push_back(path);
	}
	Dictionary work;
	work["files"] = files;
	work["moves"] = PackedStringArray(offline_moves);
	return work;
}

Dictionary PerforceWriteHook::take_offline_work() {
	Dictionary work = get_offline_work();
	MutexLock lock(pending_mutex);
	offline_changes.clear();
	offline_moves.clear();
	offline_dirty = true;
	return work;
}

void PerforceWriteHook::add_offline_work(const Dictionary &p_work) {
	MutexLock lock(pending_mutex);
	const PackedStringArray files = p_work.get("files", PackedStringArray());
	for (const String &path : files) {
		offline_changes.insert(path);
	}
	const PackedStringArray moves = p_work.get("moves", PackedStringArray());
	for (int i = 0; i + 1 < moves.size(); i += 2) {
		offline_moves.push_back(moves[i]);
		offline_moves.push_back(moves[i + 1]);
	}
	offline_dirty = true;
}

int PerforceWriteHook::get_offline_work_count() {
	MutexLock lock(pending_mutex);
	return offline_changes.size() + offline_moves.size() / 2;
}

bool PerforceWriteHook::take_offline_dirty() {
	MutexLock lock(pending_mutex);
	const bool dirty = offline_dirty;
	offline_dirty = false;
	return dirty;
}

void PerforceWriteHook::set_auto_checkout(bool p_enabled) {
	auto_checkout.set_to(p_enabled);
}

void PerforceWriteHook::set_known_opened(const Vector<String> &p_abs_paths) {
	MutexLock lock(pending_mutex);
	known_opened.clear();
	known_untracked.clear(); // Files may have been added since; ask again when needed.
	for (const String &path : p_abs_paths) {
		known_opened.insert(_key(_normalize(path)));
	}
}

// Path relative to the project, for messages.
String PerforceWriteHook::_display(const String &p_abs_path) const {
	return p_abs_path.substr(project_root.length());
}

void PerforceWriteHook::_log(const String &p_message, int p_level) {
	on_activity.call_deferred(p_message, p_level);
}

void PerforceWriteHook::_report(const String &p_what, const P4Result &p_result) {
	if (p_result.login_required) {
		on_login_required.call_deferred(p_result.get_messages());
		return;
	}
	if (p_result.errors.is_empty() || p_result.connection_failed) {
		return; // Going offline is reported once, by the plugin.
	}
	_log(vformat("%s failed: %s", p_what, p_result.get_messages()), PerforceDock::LOG_ERROR);
}

P4Result::Record PerforceWriteHook::_fstat(const String &p_abs_path) {
	P4Result result = P4Client::run({ "fstat", P4Client::escape(p_abs_path) });
	_went_offline(result);
	if (result.login_required) {
		_report("Checking " + _display(p_abs_path), result);
	}
	if (result.records.is_empty()) {
		return P4Result::Record();
	}
	return P4Result::Record(result.records[0]);
}

bool PerforceWriteHook::checkout(const Vector<String> &p_abs_paths) {
	Vector<String> to_edit, candidates;
	for (const String &path : p_abs_paths) {
		if (_is_managed(path) && FileAccess::exists(path) && P4Client::is_read_only(path)) {
			to_edit.push_back(P4Client::escape(path));
			candidates.push_back(path);
		}
	}
	if (to_edit.is_empty()) {
		return true;
	}

	P4Result result = P4Client::run_files({ "edit" }, to_edit);
	_report("Checking out files", result);
	if (_went_offline(result)) {
		// The server just went away: don't block the save, check them out later.
		for (const String &path : candidates) {
			P4Client::make_writable(path);
			_record_offline(path);
		}
		return true;
	}
	if (result.login_required) {
		return false;
	}

	// Anything still read-only couldn't be checked out (exclusively locked by
	// someone else, not in the depot, ...). The save will fail, so say why.
	Vector<String> still_read_only, checked_out;
	for (const String &path : candidates) {
		if (P4Client::is_read_only(path)) {
			still_read_only.push_back(_display(path));
		} else {
			checked_out.push_back(_display(path));
		}
	}
	if (!checked_out.is_empty()) {
		_log("Checked out " + _join_names(checked_out, "%d files"), PerforceDock::LOG_INFO);
		MutexLock lock(pending_mutex);
		for (const String &path : candidates) {
			if (!P4Client::is_read_only(path)) {
				known_opened.insert(_key(path));
			}
		}
	}
	if (!still_read_only.is_empty() && result.errors.is_empty()) {
		// Exclusive locks report "//depot/x - also opened by bob@bobs_workspace".
		Vector<String> holders;
		for (const String &info : result.infos) {
			int at = info.find("also opened by ");
			if (at != -1) {
				const String holder = info.substr(at + 15).get_slicec('@', 0);
				if (!holders.has(holder)) {
					holders.push_back(holder);
				}
			}
		}
		if (!holders.is_empty()) {
			_log(vformat("%s is locked by %s, so it can't be saved. Ask them to submit or revert it.", _join_names(still_read_only, "%d files"), String(", ").join(holders)), PerforceDock::LOG_ERROR);
		} else {
			_log(vformat("Could not check out %s, so it can't be saved. %s", _join_names(still_read_only, "%d files"), result.get_messages()), PerforceDock::LOG_ERROR);
		}
	}
	return still_read_only.is_empty();
}

bool PerforceWriteHook::has_pending() {
	MutexLock lock(pending_mutex);
	return !pending_adds.is_empty() || !pending_edits.is_empty();
}

void PerforceWriteHook::flush_pending() {
	_flush_adds();
	_flush_edits();
}

// Checks out files that were saved while already writable (P4V "allwrite"
// workspaces, files made writable by hand, .import/.uid files), so the
// changes end up in the changelist.
void PerforceWriteHook::_flush_edits() {
	Vector<String> to_edit;
	{
		MutexLock lock(pending_mutex);
		for (const String &path : pending_edits) {
			to_edit.push_back(path);
		}
		pending_edits.clear();
	}
	if (to_edit.is_empty()) {
		return;
	}
	if (offline.is_set()) {
		for (const String &path : to_edit) {
			_record_offline(path);
		}
		return;
	}

	Vector<String> escaped;
	for (const String &path : to_edit) {
		escaped.push_back(P4Client::escape(path));
	}
	P4Result result = P4Client::run_files({ "edit" }, escaped);
	_report("Checking out files", result);
	if (_went_offline(result)) {
		for (const String &path : to_edit) {
			_record_offline(path);
		}
		return;
	}
	if (result.login_required) {
		return;
	}

	HashSet<String> opened_now;
	for (const P4Result::Record &rec : result.records) {
		opened_now.insert(_key(_normalize(P4Result::field(rec, "clientFile"))));
	}

	Vector<String> checked_out, locked;
	MutexLock lock(pending_mutex);
	for (const String &path : to_edit) {
		const String key = _key(path);
		const String name = path.get_file();
		bool untracked = false;
		for (const String &warning : result.warnings) {
			if (warning.contains(name) && (warning.contains("not on client") || warning.contains("not in client view") || warning.contains("no such file") || warning.contains("can't edit exclusive"))) {
				untracked = !warning.contains("can't edit exclusive");
				if (warning.contains("can't edit exclusive")) {
					locked.push_back(_display(path));
				}
			}
		}
		if (opened_now.has(key)) {
			known_opened.insert(key);
			checked_out.push_back(_display(path));
		} else if (untracked) {
			known_untracked.insert(key);
		} else if (!locked.has(_display(path))) {
			known_opened.insert(key); // "currently opened for edit/add".
		}
	}
	if (!checked_out.is_empty()) {
		_log("Checked out " + _join_names(checked_out, "%d files"), PerforceDock::LOG_INFO);
	}
	if (!locked.is_empty()) {
		_log(vformat("%s is locked by someone else, so your changes to it can't be submitted until they submit or revert it.", _join_names(locked, "%d files")), PerforceDock::LOG_WARNING);
	}
}

void PerforceWriteHook::_flush_adds() {
	Vector<String> to_add;
	{
		MutexLock lock(pending_mutex);
		Vector<String> expired;
		for (KeyValue<String, int> &E : pending_adds) {
			if (FileAccess::exists(E.key)) {
				to_add.push_back(E.key);
			} else if (--E.value <= 0) {
				expired.push_back(E.key);
			}
		}
		for (const String &path : to_add) {
			pending_adds.erase(path);
		}
		for (const String &path : expired) {
			pending_adds.erase(path);
		}
	}
	if (to_add.is_empty()) {
		return;
	}
	if (offline.is_set()) {
		for (const String &path : to_add) {
			_record_offline(path);
		}
		return;
	}
	// -f allows names with wildcard characters (e.g. "icon@2x.png"); paths are passed unescaped.
	// Files excluded by P4IGNORE are skipped by p4 with a warning.
	P4Result result = P4Client::run_files({ "add", "-f" }, to_add);
	_report("Adding new files", result);
	if (_went_offline(result)) {
		for (const String &path : to_add) {
			_record_offline(path);
		}
		return;
	}

	// Tagged output: one record per file opened for add.
	Vector<String> added;
	for (const P4Result::Record &rec : result.records) {
		if (P4Result::field(rec, "action") == "add") {
			added.push_back(_normalize(P4Result::field(rec, "clientFile")).get_file());
		}
	}
	if (!added.is_empty()) {
		_log("Marked as new: " + _join_names(added, "%d files"), PerforceDock::LOG_INFO);
	}
	MutexLock lock(pending_mutex);
	for (const String &path : to_add) {
		known_opened.insert(_key(path));
	}
}

Error PerforceWriteHook::before_write(const String &p_path) {
	if (suppressed) {
		return OK;
	}
	const String path = _normalize(p_path);
	if (_unlock_cache_file(path) || !_is_managed(path)) {
		return OK;
	}
	const bool automatic = auto_checkout.is_set();
	if (offline.is_set() && automatic) {
		// No server: never wait. Make it writable and hand it to Perforce later.
		if (FileAccess::exists(path) && P4Client::is_read_only(path)) {
			P4Client::make_writable(path);
		}
		_record_offline(path);
		return OK;
	}
	if (!FileAccess::exists(path)) {
		if (automatic) {
			// p4 can only add files that exist; queue it until the file has been written.
			MutexLock lock(pending_mutex);
			pending_adds[path] = ADD_ATTEMPTS;
		}
		return OK;
	}

	if (P4Client::is_read_only(path)) {
		if (_is_metadata(path)) {
			// Never let .import/.uid writes fail; check them out in the background.
			P4Client::make_writable(path);
			if (automatic) {
				_queue_edit(path);
			}
			return OK;
		}
		if (!automatic) {
			_log(vformat("%s isn't checked out, so it can't be saved. Right-click it in the FileSystem dock > Perforce: Check Out (automatic checkout is off).", _display(path)), PerforceDock::LOG_ERROR);
			return ERR_FILE_NO_PERMISSION;
		}
		// Refuse up front if checkout fails (locked by someone else), instead of
		// failing later with a confusing "safe save failed" and a stray temp file.
		return checkout({ path }) ? OK : ERR_FILE_NO_PERMISSION;
	}

	// Writable but maybe not checked out (P4V "allwrite" workspaces, files made
	// writable by hand): make sure Perforce knows, without slowing the save down.
	if (automatic) {
		_queue_edit(path);
	}
	return OK;
}

Error PerforceWriteHook::before_remove(const String &p_path) {
	if (suppressed) {
		return ERR_SKIP;
	}
	const String path = _normalize(p_path);
	if (_unlock_cache_file(path) || !_is_managed(path) || !FileAccess::exists(path)) {
		return ERR_SKIP; // Directories, cache and unmanaged files are removed normally.
	}
	{
		MutexLock lock(pending_mutex);
		pending_adds.erase(path);
	}

	P4Result::Record stat;
	if (!offline.is_set()) {
		stat = _fstat(path);
	}
	if (offline.is_set()) {
		// Delete it locally (read-only files can't be deleted on Windows); Perforce gets it later.
		if (P4Client::is_read_only(path)) {
			P4Client::make_writable(path);
		}
		_record_offline(path);
		return ERR_SKIP;
	}
	if (stat.is_empty()) {
		return ERR_SKIP; // Not in Perforce.
	}

	const String action = P4Result::field(stat, "action");
	const String esc = P4Client::escape(path);

	if (action == "add") {
		// Never submitted: just forget about it.
		_report("Reverting " + _display(path), P4Client::run({ "revert", esc }));
		_log("Removed new file " + _display(path), PerforceDock::LOG_INFO);
		return ERR_SKIP;
	}
	if (action == "move/add") {
		// Deleting a moved file deletes the original.
		const String original = P4Result::field(stat, "movedFile");
		_report("Reverting " + _display(path), P4Client::run({ "revert", esc }));
		if (!original.is_empty()) {
			_report("Deleting " + original.get_file(), P4Client::run({ "delete", original }));
		}
		_log("Deleted " + _display(path), PerforceDock::LOG_INFO);
		return FileAccess::exists(path) ? ERR_SKIP : OK;
	}
	if (_is_deleted_action(action) || (action.is_empty() && _is_deleted_action(P4Result::field(stat, "headAction")))) {
		return ERR_SKIP;
	}
	if (!action.is_empty()) {
		// Opened for edit, integrate, ...: drop the local changes first.
		_report("Reverting " + _display(path), P4Client::run({ "revert", esc }));
	}

	P4Result result = P4Client::run({ "delete", esc });
	_report("Deleting " + _display(path), result);
	if (!FileAccess::exists(path)) {
		_log("Deleted " + _display(path), PerforceDock::LOG_INFO);
		return OK;
	}
	return result.ok() ? ERR_SKIP : FAILED;
}

// Moves unversioned leftovers from p_from into p_to and removes p_from.
// Called with hooks suppressed, so these are plain file system operations.
Error PerforceWriteHook::_move_remaining(const String &p_from, const String &p_to) {
	if (!DirAccess::dir_exists_absolute(p_from)) {
		return OK;
	}
	Ref<DirAccess> da = DirAccess::open(p_from);
	ERR_FAIL_COND_V(da.is_null(), FAILED);
	Error err = DirAccess::make_dir_recursive_absolute(p_to);
	if (err != OK && err != ERR_ALREADY_EXISTS) {
		return err;
	}

	for (const String &dir : da->get_directories()) {
		err = _move_remaining(p_from.path_join(dir), p_to.path_join(dir));
		if (err != OK) {
			return err;
		}
	}
	for (const String &file : da->get_files()) {
		err = da->rename(p_from.path_join(file), p_to.path_join(file));
		if (err != OK) {
			return err;
		}
	}
	da.unref();
	return DirAccess::remove_absolute(p_from);
}

Error PerforceWriteHook::before_rename(const String &p_from, const String &p_to) {
	if (suppressed) {
		return ERR_SKIP;
	}
	const String from = _normalize(p_from);
	const String to = _normalize(p_to);
	if (_unlock_cache_file(from)) {
		_unlock_cache_file(to); // Renames over an existing cache file need it writable too.
		return ERR_SKIP;
	}
	if (!_is_managed(from) || !_is_managed(to)) {
		return ERR_SKIP;
	}

	if (offline.is_set()) {
		// Rename locally; it becomes a `p4 move` (keeping history) once the server is back.
		MutexLock lock(pending_mutex);
		pending_adds.erase(from);
		offline_moves.push_back(from);
		offline_moves.push_back(to);
		offline_dirty = true;
		return ERR_SKIP;
	}

	if (DirAccess::dir_exists_absolute(from)) {
		P4Result tracked = P4Client::run({ "fstat", "-m1", P4Client::escape(from) + "/..." });
		if (_went_offline(tracked)) {
			MutexLock lock(pending_mutex);
			offline_moves.push_back(from);
			offline_moves.push_back(to);
			offline_dirty = true;
			return ERR_SKIP;
		}
		if (tracked.records.is_empty()) {
			return ERR_SKIP; // Nothing versioned inside.
		}
		// Let Perforce move the versioned files (keeps their history), then move whatever is left.
		const String esc_from = P4Client::escape(from) + "/...";
		P4Result edit = P4Client::run({ "edit", esc_from });
		_report("Checking out folder " + _display(from), edit);
		P4Result result = P4Client::run({ "move", esc_from, P4Client::escape(to) + "/..." });
		_report("Moving folder " + _display(from), result);
		if (!result.ok()) {
			return FAILED;
		}
		suppressed = true;
		Error err = _move_remaining(from, to);
		suppressed = false;
		if (err == OK) {
			_log(vformat("Moved folder %s to %s", _display(from), _display(to)), PerforceDock::LOG_INFO);
		}
		return err;
	}

	P4Result::Record stat = _fstat(from);
	if (offline.is_set()) {
		MutexLock lock(pending_mutex);
		offline_moves.push_back(from);
		offline_moves.push_back(to);
		offline_dirty = true;
		return ERR_SKIP;
	}
	const String action = P4Result::field(stat, "action");
	if (stat.is_empty() || _is_deleted_action(action) || (action.is_empty() && _is_deleted_action(P4Result::field(stat, "headAction")))) {
		// Not versioned: a normal rename, and the new name is a new file.
		MutexLock lock(pending_mutex);
		pending_adds.erase(from);
		pending_adds[to] = ADD_ATTEMPTS;
		return ERR_SKIP;
	}

	const String esc_from = P4Client::escape(from);
	if (action.is_empty()) {
		P4Result result = P4Client::run({ "edit", esc_from });
		_report("Checking out " + _display(from), result);
		if (!result.ok()) {
			return FAILED;
		}
	}

	P4Result result = P4Client::run({ "move", esc_from, P4Client::escape(to) });
	_report("Moving " + _display(from), result);
	if (FileAccess::exists(to) && !FileAccess::exists(from)) {
		_log(vformat("Moved %s to %s", _display(from), _display(to)), PerforceDock::LOG_INFO);
		return OK;
	}
	return FAILED;
}

void PerforceWriteHook::prepare_writes(const Vector<String> &p_paths) {
	if (suppressed) {
		return;
	}
	// Called before a reimport rewrites these .import files: make them writable
	// right away and check them out in the background, all in one command.
	for (const String &p : p_paths) {
		const String path = _normalize(p);
		if (!_is_managed(path) || !FileAccess::exists(path)) {
			continue;
		}
		if (P4Client::is_read_only(path)) {
			P4Client::make_writable(path);
		}
		if (auto_checkout.is_set()) {
			if (offline.is_set()) {
				_record_offline(path);
			} else {
				_queue_edit(path);
			}
		}
	}
}

PerforceWriteHook::PerforceWriteHook(const String &p_project_root, const Callable &p_on_login_required, const Callable &p_on_activity, const Callable &p_on_offline) {
	project_root = p_project_root.replace_char('\\', '/').simplify_path();
	if (!project_root.ends_with("/")) {
		project_root += "/";
	}
#ifdef WINDOWS_ENABLED
	project_root = project_root.to_lower();
#endif
	ignored_root = project_root + ".godot/";
	on_login_required = p_on_login_required;
	on_activity = p_on_activity;
	on_offline = p_on_offline;
	auto_checkout.set();
}
