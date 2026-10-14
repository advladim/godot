/**************************************************************************/
/*  p4_client.h                                                           */
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

#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/list.h"
#include "core/templates/vector.h"

// Result of one `p4 -s -ztag` invocation.
struct P4Result {
	typedef HashMap<String, String> Record;

	Vector<Record> records; // Tagged output, one entry per file/object.
	Vector<String> infos; // Untagged "info:" lines.
	Vector<String> warnings;
	Vector<String> errors;
	int exit_code = -1;
	bool login_required = false;
	bool connection_failed = false;
	bool not_installed = false; // The p4 executable could not be started.

	bool ok() const { return exit_code == 0 && errors.is_empty(); }
	String get_messages() const;

	// Value of a field in a record, or an empty string.
	static String field(const Record &p_record, const String &p_key) {
		const String *value = p_record.getptr(p_key);
		return value ? *value : String();
	}
};

// Connection used for every command. Empty fields fall back to the
// environment, `p4 set` and P4CONFIG, the same as P4V and the p4 CLI.
struct P4Connection {
	String p4_path = "p4";
	String port;
	String user;
	String client;
	String cwd; // Used for P4CONFIG lookup.
};

// Thin wrapper around the p4 command line client. Thread-safe.
class P4Client {
	static Mutex mutex;
	static P4Connection connection;

	static List<String> _global_args(const P4Connection &p_conn, bool p_tagged = true);
	static void _parse(const String &p_output, P4Result &r_result);

public:
	static void set_connection(const P4Connection &p_connection);
	static P4Connection get_connection();

	// Runs `p4 <args>` and parses the output. File arguments must already be escaped.
	static P4Result run(const Vector<String> &p_args);
	// Runs the same command once per chunk of files, merging the results.
	static P4Result run_files(const Vector<String> &p_args, const Vector<String> &p_files, int p_chunk_size = 100);
	// Contents of a depot file revision (`p4 print -q`), e.g. "//depot/a.tscn#3".
	static Error print(const String &p_file_rev, String &r_content);
	// Runs `p4 login` with the password on stdin. The password is not stored.
	static P4Result login(const String &p_password);

	// Effective value of a P4 variable (P4PORT, P4USER, ...) from the environment, `p4 set` or P4CONFIG.
	static String get_variable(const String &p_name);
	// Sets a P4 variable for this user (`p4 set name=value`, stored by p4/P4V).
	static Error set_variable(const String &p_name, const String &p_value);

	// Whether Perforce has the file locally read-only (not checked out). Windows
	// uses the read-only attribute; Linux/macOS use the owner write permission.
	static bool is_read_only(const String &p_path);
	static void make_writable(const String &p_path);
	static void make_read_only(const String &p_path);

	// Escapes the characters Perforce treats as wildcards/revision specifiers (@ # % *).
	static String escape(const String &p_path);
};
