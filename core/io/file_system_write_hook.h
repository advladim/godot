/**************************************************************************/
/*  file_system_write_hook.h                                              */
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

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

// Lets the editor react to file system changes before they happen, for example
// to check files out of a version control system that keeps them read-only
// (Perforce). Only installed in editor builds; paths are passed as received
// (`res://`, absolute or OS-fixed) and the hook is responsible for normalizing them.
// Hooks may be called from any thread.
class FileSystemWriteHook {
	static FileSystemWriteHook *singleton;

public:
	static FileSystemWriteHook *get_singleton() { return singleton; }
	static void set_singleton(FileSystemWriteHook *p_hook) { singleton = p_hook; }

	// Called before `p_path` is opened for writing. The file may not exist yet.
	// Return an error to refuse the write (e.g. the file is locked by someone else).
	virtual Error before_write(const String &p_path) = 0;

	// Called before `p_path` is removed. Return OK if the hook removed the file
	// itself, ERR_SKIP to let the caller remove it, or any other error to abort.
	virtual Error before_remove(const String &p_path) = 0;

	// Same contract as before_remove(), for renames and moves.
	virtual Error before_rename(const String &p_from, const String &p_to) = 0;

	// Hint that the given files are about to be written, so the hook can prepare
	// them all at once instead of one by one (e.g. before a reimport).
	virtual void prepare_writes(const Vector<String> &p_paths) {}

	virtual ~FileSystemWriteHook() {}
};
