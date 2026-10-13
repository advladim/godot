/**************************************************************************/
/*  perforce_write_hook.h                                                 */
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

#include "core/io/file_system_write_hook.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/variant/callable.h"

// Keeps Perforce in sync with what the editor does to project files:
// checks out read-only files before they are written, marks new files for
// add, and turns deletes and renames into `p4 delete` / `p4 move`.
class PerforceWriteHook : public FileSystemWriteHook {
	String project_root; // Absolute, forward slashes, ends with '/'.
	String ignored_root; // project_root + ".godot/".

	Mutex pending_mutex;
	HashMap<String, int> pending_adds; // Path -> attempts left.

	Callable on_login_required; // Called deferred with the p4 error message.
	Callable on_activity; // Called deferred with (message, PerforceDock::LogLevel).

	String _normalize(const String &p_path) const;
	bool _is_managed(const String &p_abs_path) const;
	bool _unlock_cache_file(const String &p_abs_path) const;
	String _display(const String &p_abs_path) const;
	void _log(const String &p_message, int p_level);
	void _report(const String &p_what, const P4Result &p_result);
	P4Result::Record _fstat(const String &p_abs_path);
	Error _move_remaining(const String &p_from, const String &p_to);

public:
	// Not-yet-existing files queued for `p4 add` are flushed here. Call on the main thread.
	void flush_pending_adds();
	// Checks out every managed, read-only file in the list with a single command.
	// Returns false if some of them are still read-only afterwards.
	bool checkout(const Vector<String> &p_abs_paths);

	virtual Error before_write(const String &p_path) override;
	virtual Error before_remove(const String &p_path) override;
	virtual Error before_rename(const String &p_from, const String &p_to) override;
	virtual void prepare_writes(const Vector<String> &p_paths) override;

	// While true on the current thread, hooks do nothing (used for our own file operations).
	static thread_local bool suppressed;

	PerforceWriteHook(const String &p_project_root, const Callable &p_on_login_required, const Callable &p_on_activity);
};
