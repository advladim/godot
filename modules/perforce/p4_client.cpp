/**************************************************************************/
/*  p4_client.cpp                                                         */
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

#include "p4_client.h"

#include "core/io/file_access.h"
#include "core/os/os.h"

Mutex P4Client::mutex;
P4Connection P4Client::connection;

String P4Result::get_messages() const {
	Vector<String> lines;
	lines.append_array(errors);
	lines.append_array(warnings);
	return String("\n").join(lines);
}

void P4Client::set_connection(const P4Connection &p_connection) {
	MutexLock lock(mutex);
	connection = p_connection;
}

P4Connection P4Client::get_connection() {
	MutexLock lock(mutex);
	return connection;
}

List<String> P4Client::_global_args(const P4Connection &p_conn, bool p_tagged) {
	List<String> args;
	if (p_tagged) {
		args.push_back("-s"); // Prefix every line with its type ("info1:", "error:", "exit:").
		args.push_back("-ztag");
	}
	// Don't hang the editor forever when the server is unreachable.
	args.push_back("-vnet.maxwait=15");
	// Always honor the project's .p4ignore (keeps the .godot cache out of the
	// depot), even if the user never ran `p4 set P4IGNORE=.p4ignore`.
	args.push_back("-E");
	args.push_back("P4IGNORE=.p4ignore");
	if (!p_conn.cwd.is_empty()) {
		args.push_back("-d");
		args.push_back(p_conn.cwd);
	}
	if (!p_conn.port.is_empty()) {
		args.push_back("-p");
		args.push_back(p_conn.port);
	}
	if (!p_conn.user.is_empty()) {
		args.push_back("-u");
		args.push_back(p_conn.user);
	}
	if (!p_conn.client.is_empty()) {
		args.push_back("-c");
		args.push_back(p_conn.client);
	}
	return args;
}

void P4Client::_parse(const String &p_output, P4Result &r_result) {
	P4Result::Record record;

	Vector<String> lines = p_output.replace("\r", "").split("\n", false);
	for (const String &line : lines) {
		if (line.begins_with("info1: ")) {
			String rest = line.substr(7);
			int space = rest.find_char(' ');
			String key = space == -1 ? rest : rest.substr(0, space);
			String value = space == -1 ? String() : rest.substr(space + 1);
			if (key.begins_with("//")) {
				// Untagged message about a depot file, e.g. "//depot/a.png - also opened by bob@ws".
				r_result.infos.push_back(rest);
				continue;
			}
			// A repeated key means a new record has started.
			if (record.has(key)) {
				r_result.records.push_back(P4Result::Record(record));
				record.clear();
			}
			record[key] = value;
		} else if (line.begins_with("info")) {
			// "info:" or deeper levels ("info2:").
			int colon = line.find_char(':');
			r_result.infos.push_back(line.substr(colon + 2));
		} else if (line.begins_with("warning: ")) {
			r_result.warnings.push_back(line.substr(9));
		} else if (line.begins_with("error: ")) {
			r_result.errors.push_back(line.substr(7));
		} else if (line.begins_with("exit: ")) {
			r_result.exit_code = line.substr(6).to_int();
		} else if (!line.strip_edges().is_empty()) {
			// Unprefixed output means p4 itself failed (e.g. not installed).
			r_result.errors.push_back(line);
		}
	}
	if (!record.is_empty()) {
		r_result.records.push_back(P4Result::Record(record));
	}

	for (const String &error : r_result.errors) {
		if (error.contains("P4PASSWD") || error.contains("session has expired") || error.contains("please login again")) {
			r_result.login_required = true;
		}
		if (error.contains("Connect to server failed") || error.contains("TCP connect to")) {
			r_result.connection_failed = true;
		}
	}
}

P4Result P4Client::run(const Vector<String> &p_args) {
	P4Connection conn = get_connection();
	List<String> args = _global_args(conn);
	for (const String &arg : p_args) {
		args.push_back(arg);
	}

	P4Result result;
	String output;
	int exit_code = -1;
	Error err = OS::get_singleton()->execute(conn.p4_path, args, &output, &exit_code, true);
	if (err != OK) {
		result.errors.push_back(vformat("Could not run \"%s\". Is the Perforce command line client (p4) installed and on PATH?", conn.p4_path));
		result.not_installed = true;
		return result;
	}
	_parse(output, result);
	if (result.exit_code == -1) {
		result.exit_code = exit_code;
	}
	return result;
}

P4Result P4Client::run_files(const Vector<String> &p_args, const Vector<String> &p_files, int p_chunk_size) {
	P4Result merged;
	merged.exit_code = 0;
	for (int i = 0; i < p_files.size(); i += p_chunk_size) {
		Vector<String> args = p_args;
		args.append_array(p_files.slice(i, MIN(i + p_chunk_size, p_files.size())));
		P4Result chunk = run(args);
		for (const P4Result::Record &record : chunk.records) {
			merged.records.push_back(P4Result::Record(record));
		}
		merged.infos.append_array(chunk.infos);
		merged.warnings.append_array(chunk.warnings);
		merged.errors.append_array(chunk.errors);
		merged.login_required = merged.login_required || chunk.login_required;
		merged.connection_failed = merged.connection_failed || chunk.connection_failed;
		if (chunk.exit_code != 0) {
			merged.exit_code = chunk.exit_code;
		}
	}
	return merged;
}

P4Result P4Client::login(const String &p_password) {
	P4Connection conn = get_connection();
	List<String> args = _global_args(conn);
	args.push_back("login");

	P4Result result;
	Dictionary pipes = OS::get_singleton()->execute_with_pipe(conn.p4_path, args, true);
	Ref<FileAccess> stdio = pipes.get("stdio", Ref<FileAccess>());
	if (stdio.is_null()) {
		result.errors.push_back(vformat("Could not run \"%s\".", conn.p4_path));
		return result;
	}

	stdio->store_string(p_password + "\n");
	stdio->flush();

	// Read until the process exits and closes its end of the pipe.
	PackedByteArray bytes;
	uint8_t byte = 0;
	while (stdio->get_buffer(&byte, 1) == 1 && stdio->get_error() == OK) {
		bytes.push_back(byte);
	}
	stdio->close();

	int64_t pid = pipes.get("pid", -1);
	while (pid != -1 && OS::get_singleton()->is_process_running(pid)) {
		OS::get_singleton()->delay_usec(10000);
	}

	String output = String::utf8((const char *)bytes.ptr(), bytes.size());
	// The password prompt has no prefix; don't treat it as an error.
	output = output.replace("Enter password:", "");
	_parse(output, result);
	return result;
}

Error P4Client::print(const String &p_file_rev, String &r_content) {
	P4Connection conn = get_connection();
	List<String> args = _global_args(conn, false);
	args.push_back("print");
	args.push_back("-q");
	args.push_back(p_file_rev);

	int exit_code = -1;
	Error err = OS::get_singleton()->execute(conn.p4_path, args, &r_content, &exit_code);
	if (err != OK) {
		return err;
	}
	return exit_code == 0 ? OK : FAILED;
}

String P4Client::get_variable(const String &p_name) {
	P4Connection conn = get_connection();
	List<String> args;
	if (!conn.cwd.is_empty()) {
		args.push_back("-d");
		args.push_back(conn.cwd);
	}
	args.push_back("set");
	args.push_back("-q");
	args.push_back(p_name);

	String output;
	int exit_code = -1;
	if (OS::get_singleton()->execute(conn.p4_path, args, &output, &exit_code) != OK) {
		return String();
	}
	// Prints "P4PORT=value".
	for (const String &line : output.replace("\r", "").split("\n", false)) {
		if (line.begins_with(p_name + "=")) {
			return line.substr(p_name.length() + 1).strip_edges();
		}
	}
	return String();
}

Error P4Client::set_variable(const String &p_name, const String &p_value) {
	P4Connection conn = get_connection();
	List<String> args;
	args.push_back("set");
	args.push_back(p_name + "=" + p_value);
	int exit_code = -1;
	Error err = OS::get_singleton()->execute(conn.p4_path, args, nullptr, &exit_code);
	return err != OK ? err : (exit_code == 0 ? OK : FAILED);
}

bool P4Client::is_read_only(const String &p_path) {
#ifdef WINDOWS_ENABLED
	return FileAccess::get_read_only_attribute(p_path);
#else
	// FileAccess::get_read_only_attribute() only reports the BSD/macOS immutable flag.
	return !FileAccess::get_unix_permissions(p_path).has_flag(FileAccess::UNIX_WRITE_OWNER);
#endif
}

void P4Client::make_writable(const String &p_path) {
#ifdef WINDOWS_ENABLED
	FileAccess::set_read_only_attribute(p_path, false);
#else
	BitField<FileAccess::UnixPermissionFlags> permissions = FileAccess::get_unix_permissions(p_path);
	permissions.set_flag(FileAccess::UNIX_WRITE_OWNER);
	FileAccess::set_unix_permissions(p_path, permissions);
#endif
}

String P4Client::escape(const String &p_path) {
	return p_path.replace("%", "%25").replace("@", "%40").replace("#", "%23").replace("*", "%2A");
}
