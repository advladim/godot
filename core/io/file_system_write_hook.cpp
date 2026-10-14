/**************************************************************************/
/*  file_system_write_hook.cpp                                            */
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

#include "file_system_write_hook.h"

#include "core/io/file_access.h"

#include <cstring>

FileSystemWriteHook *FileSystemWriteHook::singleton = nullptr;

// Text files compared without regard to line endings: version control can check
// them out with CRLF on Windows while the editor always writes LF.
static bool _text_equal_ignoring_line_endings(const Ref<FileAccess> &p_a, const Ref<FileAccess> &p_b) {
	const uint64_t max_size = 32 * 1024 * 1024;
	if (p_a->get_length() > max_size || p_b->get_length() > max_size) {
		return false;
	}
	p_a->seek(0);
	p_b->seek(0);
	const Vector<uint8_t> a = p_a->get_buffer(p_a->get_length());
	const Vector<uint8_t> b = p_b->get_buffer(p_b->get_length());
	// Only for text: binary data must match byte for byte.
	const int64_t probe_a = MIN(a.size(), (int64_t)8192), probe_b = MIN(b.size(), (int64_t)8192);
	if (memchr(a.ptr(), 0, probe_a) || memchr(b.ptr(), 0, probe_b)) {
		return false;
	}
	int64_t i = 0, j = 0;
	while (i < a.size() && j < b.size()) {
		if (a[i] == '\r' && i + 1 < a.size() && a[i + 1] == '\n') {
			i++;
		}
		if (b[j] == '\r' && j + 1 < b.size() && b[j + 1] == '\n') {
			j++;
		}
		if (a[i] != b[j]) {
			return false;
		}
		i++;
		j++;
	}
	return i == a.size() && j == b.size();
}

bool FileSystemWriteHook::files_equal(const String &p_a, const String &p_b) {
	Ref<FileAccess> a = FileAccess::open(p_a, FileAccess::READ);
	Ref<FileAccess> b = FileAccess::open(p_b, FileAccess::READ);
	if (a.is_null() || b.is_null()) {
		return false;
	}
	if (a->get_length() != b->get_length()) {
		return _text_equal_ignoring_line_endings(a, b);
	}
	const uint64_t chunk = 64 * 1024;
	Vector<uint8_t> buffer_a, buffer_b;
	buffer_a.resize(chunk);
	buffer_b.resize(chunk);
	uint64_t remaining = a->get_length();
	while (remaining > 0) {
		const uint64_t size = MIN(chunk, remaining);
		if (a->get_buffer(buffer_a.ptrw(), size) != size || b->get_buffer(buffer_b.ptrw(), size) != size) {
			return false;
		}
		if (memcmp(buffer_a.ptr(), buffer_b.ptr(), size) != 0) {
			// Same length can still differ only in line endings (e.g. a lone \r somewhere); rare, keep it exact.
			return false;
		}
		remaining -= size;
	}
	return true;
}
