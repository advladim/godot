/**************************************************************************/
/*  scene_merger.cpp                                                      */
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

#include "scene_merger.h"

#include "core/io/file_access.h"
#include "core/string/print_string.h"
#include "core/templates/hash_set.h"

namespace {

struct Entry {
	String key;
	String value; // Raw text, exactly as written in the file.

	bool operator==(const Entry &p_other) const { return key == p_other.key && value == p_other.value; }
	bool operator!=(const Entry &p_other) const { return !(*this == p_other); }
};

struct Section {
	String tag;
	Vector<Entry> attrs; // Header fields: [tag key=value ...]
	Vector<Entry> props; // Body lines: key = value
	String key; // Identity used to match the section between versions.

	int find_attr(const String &p_key) const {
		for (int i = 0; i < attrs.size(); i++) {
			if (attrs[i].key == p_key) {
				return i;
			}
		}
		return -1;
	}
	bool has_attr(const String &p_key) const { return find_attr(p_key) != -1; }
	String get_attr(const String &p_key) const {
		int i = find_attr(p_key);
		return i == -1 ? String() : attrs[i].value;
	}
	void set_attr(const String &p_key, const String &p_value) {
		int i = find_attr(p_key);
		if (i == -1) {
			attrs.push_back({ p_key, p_value });
		} else {
			attrs.write[i].value = p_value;
		}
	}
	bool operator==(const Section &p_other) const { return tag == p_other.tag && attrs == p_other.attrs && props == p_other.props; }
	bool operator!=(const Section &p_other) const { return !(*this == p_other); }
};

String unquote(const String &p_value) {
	if (p_value.length() >= 2 && p_value.begins_with("\"") && p_value.ends_with("\"")) {
		return p_value.substr(1, p_value.length() - 2).c_unescape();
	}
	return p_value;
}

String quote(const String &p_value) {
	return "\"" + p_value.c_escape() + "\"";
}

// Full path of a node relative to the scene root ("." for the root itself).
String node_path(const Section &p_node) {
	const String name = unquote(p_node.get_attr("name"));
	if (!p_node.has_attr("parent")) {
		return ".";
	}
	const String parent = unquote(p_node.get_attr("parent"));
	return parent == "." ? name : parent + "/" + name;
}

void assign_key(Section &r_section) {
	const String &tag = r_section.tag;
	if (tag.is_empty()) {
		r_section.key = "#preamble"; // Settings before the first [section] (project.godot).
	} else if (tag == "gd_scene" || tag == "gd_resource") {
		r_section.key = "#header";
	} else if (tag == "resource") {
		r_section.key = "#resource";
	} else if (tag == "ext_resource") {
		r_section.key = "ext:" + unquote(r_section.has_attr("uid") ? r_section.get_attr("uid") : r_section.get_attr("path"));
	} else if (tag == "sub_resource") {
		r_section.key = "sub:" + unquote(r_section.get_attr("id"));
	} else if (tag == "node") {
		r_section.key = "node:" + node_path(r_section);
	} else if (tag == "connection") {
		r_section.key = "conn:" + unquote(r_section.get_attr("signal")) + "|" + unquote(r_section.get_attr("from")) + "|" + unquote(r_section.get_attr("to")) + "|" + unquote(r_section.get_attr("method"));
	} else if (tag == "editable") {
		r_section.key = "editable:" + unquote(r_section.get_attr("path"));
	} else {
		String header;
		for (const Entry &attr : r_section.attrs) {
			header += " " + attr.key + "=" + attr.value;
		}
		r_section.key = "other:" + tag + header;
	}
}

String describe(const Section &p_section) {
	const String &tag = p_section.tag;
	if (tag.is_empty()) {
		return "Top of file";
	} else if (tag == "gd_scene" || tag == "gd_resource") {
		return "File header";
	} else if (tag == "resource") {
		return "Resource";
	} else if (tag == "ext_resource") {
		return "External resource " + unquote(p_section.get_attr("path"));
	} else if (tag == "sub_resource") {
		return "Sub-resource " + unquote(p_section.get_attr("id"));
	} else if (tag == "node") {
		return "Node " + node_path(p_section);
	} else if (tag == "connection") {
		return vformat("Connection %s from %s to %s.%s", unquote(p_section.get_attr("signal")), unquote(p_section.get_attr("from")), unquote(p_section.get_attr("to")), unquote(p_section.get_attr("method")));
	} else if (tag == "editable") {
		return "Editable children of " + unquote(p_section.get_attr("path"));
	}
	return "[" + tag + "]";
}

class Parser {
	const String &text;
	int pos = 0;

	bool eof() const { return pos >= text.length(); }
	char32_t peek() const { return eof() ? 0 : text[pos]; }

	void skip_spaces() {
		while (!eof() && (peek() == ' ' || peek() == '\t')) {
			pos++;
		}
	}
	void skip_whitespace() {
		while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
			pos++;
		}
	}
	void skip_string() {
		pos++; // Opening quote.
		while (!eof() && peek() != '"') {
			if (peek() == '\\') {
				pos++;
			}
			pos++;
		}
		pos++; // Closing quote.
	}

	// Reads a value up to the end of the line (body) or the next space/']' (header),
	// skipping over strings and anything nested in brackets, which may span lines.
	String scan_value(bool p_in_header) {
		const int start = pos;
		int depth = 0;
		while (!eof()) {
			const char32_t c = peek();
			if (c == '"') {
				skip_string();
				continue;
			}
			if (c == '(' || c == '[' || c == '{') {
				depth++;
			} else if (c == ')' || c == ']' || c == '}') {
				if (depth == 0) {
					if (p_in_header) {
						break;
					}
				} else {
					depth--;
				}
			} else if (depth == 0 && (c == '\n' || c == '\r' || (p_in_header && c == ' '))) {
				break;
			}
			pos++;
		}
		return text.substr(start, pos - start).strip_edges(false, true);
	}

	String scan_key() {
		const int start = pos;
		if (peek() == '"') {
			skip_string();
		} else {
			while (!eof() && peek() != '=' && peek() != ' ' && peek() != '\t' && peek() != '\n' && peek() != ']') {
				pos++;
			}
		}
		return text.substr(start, pos - start);
	}

	// key = value lines up to the next section header.
	Error parse_props(Section &r_section, String &r_error) {
		while (true) {
			skip_whitespace();
			if (eof() || peek() == '[') {
				return OK;
			}
			String key = scan_key();
			skip_spaces();
			if (peek() != '=') {
				r_error = vformat("Expected '=' after \"%s\" in [%s].", key, r_section.tag);
				return ERR_PARSE_ERROR;
			}
			pos++;
			skip_spaces();
			r_section.props.push_back({ key, scan_value(false) });
		}
	}

public:
	String leading_comment; // ';' lines at the top (project.godot), kept as is.

	Error parse(Vector<Section> &r_sections, String &r_error) {
		while (true) {
			skip_whitespace();
			if (eof()) {
				return OK;
			}
			if (peek() == ';') { // Comment line.
				const int start = pos;
				while (!eof() && peek() != '\n') {
					pos++;
				}
				if (r_sections.is_empty()) {
					leading_comment += text.substr(start, pos - start).strip_edges(false, true) + "\n";
				}
				continue;
			}
			if (peek() != '[') {
				if (!r_sections.is_empty()) {
					r_error = vformat("Expected a section header at character %d.", pos);
					return ERR_PARSE_ERROR;
				}
				// Config files (project.godot) start with settings outside any section.
				Section preamble;
				if (parse_props(preamble, r_error) != OK) {
					return ERR_PARSE_ERROR;
				}
				assign_key(preamble);
				r_sections.push_back(preamble);
				continue;
			}
			pos++;

			Section section;
			section.tag = scan_key();
			while (true) {
				skip_spaces();
				if (eof()) {
					r_error = "Unterminated section header.";
					return ERR_PARSE_ERROR;
				}
				if (peek() == ']') {
					pos++;
					break;
				}
				String key = scan_key();
				skip_spaces();
				if (peek() != '=') {
					r_error = vformat("Expected '=' after \"%s\" in [%s].", key, section.tag);
					return ERR_PARSE_ERROR;
				}
				pos++;
				skip_spaces();
				section.attrs.push_back({ key, scan_value(true) });
			}

			if (parse_props(section, r_error) != OK) {
				return ERR_PARSE_ERROR;
			}

			assign_key(section);
			r_sections.push_back(section);
		}
	}

	explicit Parser(const String &p_text) :
			text(p_text) {}
};

String serialize(const Vector<Section> &p_sections) {
	String out;
	for (int i = 0; i < p_sections.size(); i++) {
		const Section &section = p_sections[i];
		// Godot writes these one per line without blank lines in between.
		const bool compact = section.tag == "ext_resource" || section.tag == "connection" || section.tag == "editable";
		if (i > 0 && !(compact && p_sections[i - 1].tag == section.tag)) {
			out += "\n";
		}
		out += "[" + section.tag;
		for (const Entry &attr : section.attrs) {
			out += " " + attr.key + "=" + attr.value;
		}
		out += "]\n";
		for (const Entry &prop : section.props) {
			out += prop.key + " = " + prop.value + "\n";
		}
	}
	return out;
}

// project.godot / export_presets.cfg style: "key=value", a blank line after each header.
String serialize_config(const String &p_leading_comment, const Vector<Section> &p_sections) {
	String out = p_leading_comment;
	for (const Section &section : p_sections) {
		if (!out.is_empty()) {
			out += "\n";
		}
		if (!section.tag.is_empty()) {
			out += "[" + section.tag + "]\n\n";
		}
		for (const Entry &prop : section.props) {
			out += prop.key + "=" + prop.value + "\n";
		}
	}
	return out;
}

// Config files (project.godot) have no [gd_scene]/[gd_resource] header.
bool is_config(const Vector<Section> &p_sections) {
	if (p_sections.is_empty()) {
		return false;
	}
	const String &tag = p_sections[0].tag;
	return tag != "gd_scene" && tag != "gd_resource";
}

// Union of two key sequences: p_ours order, with keys only in p_theirs inserted
// after the key that precedes them in p_theirs (and after anything we added there).
Vector<String> merge_order(const Vector<String> &p_ours, const Vector<String> &p_theirs) {
	Vector<String> order = p_ours;
	HashSet<String> present, in_theirs;
	for (const String &key : p_ours) {
		present.insert(key);
	}
	for (const String &key : p_theirs) {
		in_theirs.insert(key);
	}
	for (int i = 0; i < p_theirs.size(); i++) {
		if (present.has(p_theirs[i])) {
			continue;
		}
		int insert_at = 0;
		for (int j = i - 1; j >= 0; j--) {
			int found = order.find(p_theirs[j]);
			if (found != -1) {
				insert_at = found + 1;
				break;
			}
		}
		while (insert_at < order.size() && !in_theirs.has(order[insert_at])) {
			insert_at++; // Our additions at the same spot come first.
		}
		order.insert(insert_at, p_theirs[i]);
		present.insert(p_theirs[i]);
	}
	return order;
}

// Replaces ExtResource("old") / SubResource("old") references in every section.
void rename_references(Vector<Section> &r_sections, const String &p_kind, const String &p_old_id, const String &p_new_id) {
	const String from = p_kind + "(\"" + p_old_id + "\")";
	const String to = p_kind + "(\"" + p_new_id + "\")";
	for (Section &section : r_sections) {
		for (Entry &attr : section.attrs) {
			attr.value = attr.value.replace(from, to);
		}
		for (Entry &prop : section.props) {
			prop.value = prop.value.replace(from, to);
		}
	}
}

String unused_id(const String &p_id, const HashSet<String> &p_used) {
	for (int i = 1;; i++) {
		String candidate = p_id + "_m" + itos(i);
		if (!p_used.has(candidate)) {
			return candidate;
		}
	}
}

class Merger {
	const HashMap<String, SceneMerger::Choice> &choices;
	SceneMerger::Choice default_choice;

	Vector<Section> base, ours, theirs;
	String leading_comment;

	static int find_key(const Vector<Section> &p_sections, const String &p_key) {
		for (int i = 0; i < p_sections.size(); i++) {
			if (p_sections[i].key == p_key) {
				return i;
			}
		}
		return -1;
	}

	static Vector<String> keys_of(const Vector<Section> &p_sections) {
		Vector<String> keys;
		for (const Section &section : p_sections) {
			keys.push_back(section.key);
		}
		return keys;
	}

	// Nodes keep their unique_id when renamed or moved; use it to keep matching them.
	static void match_renamed_nodes(const Vector<Section> &p_base, Vector<Section> &r_side) {
		HashMap<String, String> base_key_by_uid;
		HashSet<String> side_keys;
		for (const Section &section : r_side) {
			side_keys.insert(section.key);
		}
		for (const Section &section : p_base) {
			if (section.tag == "node" && section.has_attr("unique_id")) {
				base_key_by_uid[section.get_attr("unique_id")] = section.key;
			}
		}
		for (Section &section : r_side) {
			if (section.tag != "node" || !section.has_attr("unique_id") || find_key(p_base, section.key) != -1) {
				continue;
			}
			const String *base_key = base_key_by_uid.getptr(section.get_attr("unique_id"));
			if (base_key && !side_keys.has(*base_key)) {
				section.key = *base_key;
			}
		}
	}

	// Makes resource ids in "theirs" consistent with "ours" so the merged file has no duplicates.
	void reconcile_resource_ids() {
		HashSet<String> used_ids;
		for (const Vector<Section> *sections : { &base, &ours, &theirs }) {
			for (const Section &section : *sections) {
				if (section.tag == "ext_resource" || section.tag == "sub_resource") {
					used_ids.insert(unquote(section.get_attr("id")));
				}
			}
		}

		for (int i = 0; i < theirs.size(); i++) {
			Section &section = theirs.write[i];
			const String their_id = unquote(section.get_attr("id"));

			if (section.tag == "ext_resource") {
				int in_ours = find_key(ours, section.key);
				if (in_ours != -1) {
					// Same file referenced on both sides: use our id for it.
					const String our_id = unquote(ours[in_ours].get_attr("id"));
					if (our_id != their_id) {
						section.set_attr("id", quote(our_id));
						rename_references(theirs, "ExtResource", their_id, our_id);
					}
					continue;
				}
				// A different file that happens to use an id we already use.
				bool clash = false;
				for (const Vector<Section> *sections : { &base, &ours }) {
					for (const Section &other : *sections) {
						if (other.tag == "ext_resource" && other.key != section.key && unquote(other.get_attr("id")) == their_id) {
							clash = true;
						}
					}
				}
				if (clash) {
					String new_id = unused_id(their_id, used_ids);
					used_ids.insert(new_id);
					section.set_attr("id", quote(new_id));
					rename_references(theirs, "ExtResource", their_id, new_id);
				}
			} else if (section.tag == "sub_resource") {
				// Both sides added a sub-resource with the same id (ids are seeded by
				// the scene path, so this is common) but different contents.
				if (find_key(base, section.key) != -1) {
					continue;
				}
				int in_ours = find_key(ours, section.key);
				if (in_ours != -1 && ours[in_ours] != section) {
					String new_id = unused_id(their_id, used_ids);
					used_ids.insert(new_id);
					section.set_attr("id", quote(new_id));
					section.key = "sub:" + new_id;
					rename_references(theirs, "SubResource", their_id, new_id);
				}
			}
		}
	}

	SceneMerger::Choice choose(const String &p_id) const {
		const SceneMerger::Choice *choice = choices.getptr(p_id);
		return choice ? *choice : default_choice;
	}

	Vector<Entry> merge_entries(const Vector<Entry> &p_base, const Vector<Entry> &p_ours, const Vector<Entry> &p_theirs, const String &p_id_prefix, const String &p_label, bool p_is_header) {
		auto index = [](const Vector<Entry> &p_entries) {
			HashMap<String, String> map;
			for (const Entry &entry : p_entries) {
				map[entry.key] = entry.value;
			}
			return map;
		};
		auto keys = [](const Vector<Entry> &p_entries) {
			Vector<String> list;
			for (const Entry &entry : p_entries) {
				list.push_back(entry.key);
			}
			return list;
		};
		HashMap<String, String> b = index(p_base), o = index(p_ours), t = index(p_theirs);

		Vector<Entry> merged;
		for (const String &key : merge_order(keys(p_ours), keys(p_theirs))) {
			const String *bv = b.getptr(key);
			const String *ov = o.getptr(key);
			const String *tv = t.getptr(key);
			auto same = [](const String *p_a, const String *p_b) { return (!p_a && !p_b) || (p_a && p_b && *p_a == *p_b); };

			const String *pick;
			if (same(ov, tv) || same(bv, tv)) {
				pick = ov; // Unchanged by them (or changed identically).
			} else if (same(bv, ov)) {
				pick = tv; // Only they changed it.
			} else {
				SceneMerger::Conflict conflict;
				conflict.id = p_id_prefix + key;
				conflict.section = p_label;
				conflict.property = p_is_header ? key + " (header)" : key;
				conflict.in_base = bv != nullptr;
				conflict.in_ours = ov != nullptr;
				conflict.in_theirs = tv != nullptr;
				conflict.base = bv ? *bv : String();
				conflict.ours = ov ? *ov : String();
				conflict.theirs = tv ? *tv : String();
				conflicts.push_back(conflict);
				pick = choose(conflict.id) == SceneMerger::CHOICE_THEIRS ? tv : ov;
			}
			if (pick) {
				merged.push_back({ key, *pick });
			}
		}
		return merged;
	}

	Section merge_section(const Section *p_base, const Section &p_ours, const Section &p_theirs) {
		static const Section empty;
		const Section &b = p_base ? *p_base : empty;
		const String label = describe(p_ours);
		Section merged = p_ours;
		merged.attrs = merge_entries(b.attrs, p_ours.attrs, p_theirs.attrs, p_ours.key + "|@", label, true);
		merged.props = merge_entries(b.props, p_ours.props, p_theirs.props, p_ours.key + "|", label, false);
		return merged;
	}

	void add_section_conflict(const Section *p_base, const Section *p_ours, const Section *p_theirs) {
		SceneMerger::Conflict conflict;
		conflict.id = (p_ours ? p_ours : p_theirs)->key;
		conflict.section = describe(p_ours ? *p_ours : *p_theirs);
		conflict.in_base = p_base != nullptr;
		conflict.in_ours = p_ours != nullptr;
		conflict.in_theirs = p_theirs != nullptr;
		conflict.base = p_base ? serialize({ *p_base }) : String();
		conflict.ours = p_ours ? serialize({ *p_ours }) : String("(deleted)");
		conflict.theirs = p_theirs ? serialize({ *p_theirs }) : String("(deleted)");
		conflicts.push_back(conflict);
	}

	// A node renamed/moved on one side may still be referenced by its old path
	// from sections that came from the other side (children, connections).
	void update_moved_node_paths(Vector<Section> &r_sections) {
		HashMap<String, String> moved; // Old path -> new path.
		HashSet<String> existing;
		for (const Section &section : r_sections) {
			if (section.tag != "node") {
				continue;
			}
			const String path = node_path(section);
			existing.insert(path);
			const String old_path = section.key.trim_prefix("node:");
			if (section.key.begins_with("node:") && old_path != path) {
				moved[old_path] = path;
			}
		}
		if (moved.is_empty()) {
			return;
		}

		auto remap = [&](const String &p_path) -> String {
			if (existing.has(p_path)) {
				return p_path;
			}
			for (const KeyValue<String, String> &E : moved) {
				if (p_path == E.key) {
					return E.value;
				}
				if (p_path.begins_with(E.key + "/")) {
					return E.value + p_path.substr(E.key.length());
				}
			}
			return p_path;
		};

		for (Section &section : r_sections) {
			Vector<String> fields;
			if (section.tag == "node") {
				fields.push_back("parent");
			} else if (section.tag == "connection") {
				fields.push_back("from");
				fields.push_back("to");
			} else if (section.tag == "editable") {
				fields.push_back("path");
			}
			for (const String &field : fields) {
				if (section.has_attr(field)) {
					const String path = unquote(section.get_attr(field));
					const String new_path = remap(path);
					if (new_path != path) {
						section.set_attr(field, quote(new_path));
					}
				}
			}
		}
	}

	static int rank(const String &p_tag) {
		static const char *order[] = { "gd_scene", "gd_resource", "ext_resource", "sub_resource", "resource", "node", "connection", "editable" };
		for (int i = 0; i < 8; i++) {
			if (p_tag == order[i]) {
				return i;
			}
		}
		return 8;
	}

	// Godot requires sub-resources to come after the ones they use, and parent nodes before children.
	static Vector<Section> fix_order(const Vector<Section> &p_sections) {
		Vector<Vector<Section>> groups;
		groups.resize(9);
		for (const Section &section : p_sections) {
			groups.write[rank(section.tag)].push_back(section);
		}

		// Keeps the original order, but holds a section back until p_ready() says what it depends on was emitted.
		auto stable_dependency_sort = [](const Vector<Section> &p_group, auto p_ready, auto p_name) {
			HashSet<String> all, emitted;
			for (const Section &section : p_group) {
				all.insert(p_name(section));
			}
			Vector<Section> remaining = p_group, sorted;
			bool progress = true;
			while (!remaining.is_empty() && progress) {
				progress = false;
				Vector<Section> deferred;
				for (const Section &section : remaining) {
					if (p_ready(section, all, emitted)) {
						sorted.push_back(section);
						emitted.insert(p_name(section));
						progress = true;
					} else {
						deferred.push_back(section);
					}
				}
				remaining = deferred;
			}
			sorted.append_array(remaining); // Cycles or missing parents: keep as is.
			return sorted;
		};

		// Sub-resources: all referenced SubResource ids must come first.
		groups.write[3] = stable_dependency_sort(
				groups[3],
				[](const Section &p_section, const HashSet<String> &p_all, const HashSet<String> &p_emitted) {
					const String self = unquote(p_section.get_attr("id"));
					for (const Entry &prop : p_section.props) {
						int from = 0;
						while ((from = prop.value.find("SubResource(\"", from)) != -1) {
							from += 13;
							int end = prop.value.find("\"", from);
							const String id = prop.value.substr(from, end - from);
							if (id != self && p_all.has(id) && !p_emitted.has(id)) {
								return false;
							}
						}
					}
					return true;
				},
				[](const Section &p_section) { return unquote(p_section.get_attr("id")); });

		// Nodes: parent first.
		groups.write[5] = stable_dependency_sort(
				groups[5],
				[](const Section &p_section, const HashSet<String> &p_all, const HashSet<String> &p_emitted) {
					if (!p_section.has_attr("parent")) {
						return true;
					}
					const String parent = unquote(p_section.get_attr("parent"));
					return !p_all.has(parent) || p_emitted.has(parent);
				},
				[](const Section &p_section) { return node_path(p_section); });

		Vector<Section> result;
		for (const Vector<Section> &group : groups) {
			result.append_array(group);
		}
		return result;
	}

public:
	Vector<SceneMerger::Conflict> conflicts;

	Error parse(const String &p_base, const String &p_ours, const String &p_theirs, String &r_error) {
		struct Input {
			const String &text;
			Vector<Section> &sections;
			const char *name;
		};
		for (const Input &input : { Input{ p_base, base, "base" }, Input{ p_ours, ours, "yours" }, Input{ p_theirs, theirs, "theirs" } }) {
			String error;
			Parser parser(input.text);
			if (parser.parse(input.sections, error) != OK) {
				r_error = vformat("Could not parse the %s version: %s", input.name, error);
				return ERR_PARSE_ERROR;
			}
			if (&input.sections == &ours || leading_comment.is_empty()) {
				leading_comment = parser.leading_comment; // Ours wins.
			}
		}
		return OK;
	}

	String merge() {
		match_renamed_nodes(base, ours);
		match_renamed_nodes(base, theirs);
		reconcile_resource_ids();

		Vector<Section> merged;
		for (const String &key : merge_order(keys_of(ours), keys_of(theirs))) {
			const int bi = find_key(base, key), oi = find_key(ours, key), ti = find_key(theirs, key);
			const Section *b = bi == -1 ? nullptr : &base[bi];
			const Section *o = oi == -1 ? nullptr : &ours[oi];
			const Section *t = ti == -1 ? nullptr : &theirs[ti];

			if (o && t) {
				if (*o == *t || (b && *b == *t)) {
					merged.push_back(*o);
				} else if (b && *b == *o) {
					merged.push_back(*t);
				} else {
					merged.push_back(merge_section(b, *o, *t));
				}
			} else if (o) {
				if (!b) {
					merged.push_back(*o); // We added it.
				} else if (*b != *o) {
					// We edited what they deleted.
					add_section_conflict(b, o, nullptr);
					if (choose(o->key) == SceneMerger::CHOICE_OURS) {
						merged.push_back(*o);
					}
				}
				// Otherwise they deleted it and we didn't touch it: drop.
			} else if (t) {
				if (!b) {
					merged.push_back(*t); // They added it.
				} else if (*b != *t) {
					// They edited what we deleted.
					add_section_conflict(b, nullptr, t);
					if (choose(t->key) == SceneMerger::CHOICE_THEIRS) {
						merged.push_back(*t);
					}
				}
			}
		}

		if (is_config(ours.is_empty() ? theirs : ours)) {
			return serialize_config(leading_comment, merged); // Order as written; no nodes or resources.
		}
		update_moved_node_paths(merged);
		return serialize(fix_order(merged));
	}

	Merger(const HashMap<String, SceneMerger::Choice> &p_choices, SceneMerger::Choice p_default) :
			choices(p_choices), default_choice(p_default) {}
};

} // namespace

SceneMerger::Result SceneMerger::merge(const String &p_base, const String &p_ours, const String &p_theirs, const HashMap<String, Choice> &p_choices, Choice p_default) {
	Result result;
	Merger merger(p_choices, p_default);
	// Perforce converts line endings on sync (CRLF on Windows workspaces) and
	// back on submit; merge with Godot's own LF so multi-line values stay intact.
	const String base = p_base.replace("\r\n", "\n");
	const String ours = p_ours.replace("\r\n", "\n");
	const String theirs = p_theirs.replace("\r\n", "\n");
	if (merger.parse(base, ours, theirs, result.error) != OK) {
		return result;
	}
	result.text = merger.merge();
	result.conflicts = merger.conflicts;
	return result;
}

int SceneMerger::run_command_line(const Vector<String> &p_args) {
	int index = p_args.find("--merge-scene");
	if (index == -1 || index + 4 >= p_args.size()) {
		print_error("Usage: godot --headless --merge-scene -- <base> <theirs> <yours> <result>");
		return 2;
	}
	const String base_path = p_args[index + 1], theirs_path = p_args[index + 2], ours_path = p_args[index + 3], result_path = p_args[index + 4];

	String texts[3];
	const String paths[3] = { base_path, theirs_path, ours_path };
	for (int i = 0; i < 3; i++) {
		Error err;
		texts[i] = FileAccess::get_file_as_string(paths[i], &err);
		if (err != OK) {
			print_error("Could not read " + paths[i]);
			return 2;
		}
	}

	Result result = merge(texts[0], texts[2], texts[1]);
	if (!result.error.is_empty()) {
		print_error(result.error);
		return 2;
	}

	Ref<FileAccess> file = FileAccess::open(result_path, FileAccess::WRITE);
	if (file.is_null()) {
		print_error("Could not write " + result_path);
		return 2;
	}
	file->store_string(result.text);
	file.unref();

	if (result.conflicts.is_empty()) {
		print_line("Merged cleanly.");
		return 0;
	}
	print_line(vformat("%d conflict(s); kept your version for each:", result.conflicts.size()));
	for (const Conflict &conflict : result.conflicts) {
		print_line(vformat("  %s%s\n    yours:  %s\n    theirs: %s", conflict.section, conflict.property.is_empty() ? String() : " -> " + conflict.property, conflict.in_ours ? conflict.ours : String("(missing)"), conflict.in_theirs ? conflict.theirs : String("(missing)")));
	}
	return 1;
}
