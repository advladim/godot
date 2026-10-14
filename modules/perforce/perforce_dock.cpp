/**************************************************************************/
/*  perforce_dock.cpp                                                     */
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

#include "perforce_dock.h"

#include "core/object/callable_mp.h"
#include "core/os/time.h"
#include "core/templates/hash_set.h"
#include "editor/docks/filesystem_dock.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/split_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/gui/tree.h"

static const char *_action_icon(const String &p_action) {
	if (p_action == "add" || p_action == "branch") {
		return "Add";
	} else if (p_action == "delete" || p_action == "move/delete") {
		return "Remove";
	} else if (p_action == "move/add") {
		return "Rename";
	}
	return "Edit";
}

static String _action_name(const String &p_action) {
	if (p_action == "edit") {
		return TTR("Edited");
	} else if (p_action == "add") {
		return TTR("New");
	} else if (p_action == "delete") {
		return TTR("Deleted");
	} else if (p_action == "move/add") {
		return TTR("Moved");
	} else if (p_action == "integrate") {
		return TTR("Integrated");
	}
	return p_action.capitalize();
}

void PerforceDock::_update_ui() {
	if (!is_inside_tree()) {
		return;
	}

	const bool connected = status == STATUS_CONNECTED;

	StringName icon;
	Color color;
	switch (status) {
		case STATUS_CONNECTED:
			icon = SNAME("StatusSuccess");
			color = get_theme_color(SNAME("success_color"), EditorStringName(Editor));
			break;
		case STATUS_CONNECTING:
		case STATUS_NOT_PERFORCE:
			icon = SNAME("StatusIndicator");
			color = get_theme_color(SceneStringName(font_color), EditorStringName(Editor));
			break;
		case STATUS_LOGIN_REQUIRED:
			icon = SNAME("StatusWarning");
			color = get_theme_color(SNAME("warning_color"), EditorStringName(Editor));
			break;
		case STATUS_OFFLINE:
		case STATUS_NO_P4:
			icon = SNAME("StatusError");
			color = get_theme_color(SNAME("error_color"), EditorStringName(Editor));
			break;
	}
	status_icon->set_texture(get_editor_theme_icon(icon));
	status_label->set_text(status_text);
	status_label->add_theme_color_override(SceneStringName(font_color), color);

	// The tab title is visible even when the panel is closed, so it doubles as the status indicator.
	String tab_title = TTR("Perforce");
	if (connected && opened_count > 0) {
		tab_title += vformat(" (%d)", opened_count);
	}
	set_title(tab_title);
	set_title_color(status == STATUS_CONNECTED || status == STATUS_NOT_PERFORCE || status == STATUS_CONNECTING ? Color(0, 0, 0, 0) : color);

	String remote;
	if (connected && outdated_count > 0) {
		remote = vformat(TTRN("%d newer file on server", "%d newer files on server", outdated_count), outdated_count);
	}
	outdated_label->set_text(remote);
	outdated_label->set_visible(!remote.is_empty());

	resolve_button->set_text(vformat(TTRN("Resolve %d File", "Resolve %d Files", unresolved_count), unresolved_count));
	resolve_button->set_visible(connected && unresolved_count > 0);

	get_latest_button->set_disabled(!connected || busy);
	refresh_button->set_disabled(busy);
	connection_button->set_disabled(busy);

	files_label->set_text(opened_count == 0 ? TTR("No files checked out. Saving a file checks it out automatically.") : vformat(TTRN("%d file checked out (default changelist):", "%d files checked out (default changelist):", opened_count), opened_count));
	revert_button->set_disabled(!connected || busy || files_tree->get_next_selected(nullptr) == nullptr);
	description_edit->set_editable(connected && !busy);
	submit_button->set_disabled(!connected || busy || opened_count == 0 || description_edit->get_text().strip_edges().is_empty());
	submit_button->set_tooltip_text(description_edit->get_text().strip_edges().is_empty() ? TTR("Enter a description first.") : TTR("Save all scenes and submit the checked out files of this project."));
}

void PerforceDock::_emit(const StringName &p_signal) {
	emit_signal(p_signal);
}

void PerforceDock::_files_selected() {
	_update_ui();
}

void PerforceDock::_files_activated() {
	TreeItem *item = files_tree->get_selected();
	if (item && FileSystemDock::get_singleton()) {
		FileSystemDock::get_singleton()->navigate_to_path(item->get_metadata(0));
	}
}

void PerforceDock::_revert_pressed() {
	Vector<String> paths;
	for (TreeItem *item = files_tree->get_next_selected(nullptr); item; item = files_tree->get_next_selected(item)) {
		paths.push_back(item->get_metadata(0));
	}
	if (!paths.is_empty()) {
		emit_signal(SNAME("revert_requested"), paths);
	}
}

void PerforceDock::_submit_pressed() {
	const String description = description_edit->get_text().strip_edges();
	if (!description.is_empty()) {
		emit_signal(SNAME("submit_requested"), description);
	}
}

void PerforceDock::_description_changed(const String &p_text) {
	_update_ui();
}

void PerforceDock::set_status(Status p_status, const String &p_text) {
	status = p_status;
	status_text = p_text;
	_update_ui();
}

void PerforceDock::set_busy(bool p_busy, const String &p_text) {
	busy = p_busy;
	if (p_busy && !p_text.is_empty()) {
		status_label->set_text(p_text);
	}
	_update_ui();
	if (!p_busy) {
		status_label->set_text(status_text);
	}
}

void PerforceDock::set_opened_files(const Array &p_files) {
	HashSet<String> selected;
	for (TreeItem *item = files_tree->get_next_selected(nullptr); item; item = files_tree->get_next_selected(item)) {
		selected.insert(item->get_metadata(0));
	}

	files_tree->clear();
	TreeItem *root = files_tree->create_item();
	for (const Variant &entry : p_files) {
		Dictionary file = entry;
		const String path = file["path"];
		const String action = file["action"];
		const String other = file.get("other", String());

		TreeItem *item = files_tree->create_item(root);
		item->set_icon(0, get_editor_theme_icon(_action_icon(action)));
		item->set_text(0, _action_name(action));
		item->set_text(1, path.trim_prefix("res://"));
		item->set_metadata(0, path);
		item->set_tooltip_text(1, path);
		if (!other.is_empty()) {
			// Shown after the name, only when it applies (no empty column otherwise).
			item->set_suffix(1, "  " + vformat(TTR("(also opened by %s)"), other));
			item->set_custom_color(1, get_theme_color(SNAME("warning_color"), EditorStringName(Editor)));
			item->set_tooltip_text(1, path + "\n" + vformat(TTR("Also opened by %s"), other));
		}
		if (selected.has(path)) {
			item->select(0);
		}
	}
	opened_count = p_files.size();
	_update_ui();
}

void PerforceDock::set_remote_state(int p_outdated, int p_unresolved) {
	outdated_count = p_outdated;
	unresolved_count = p_unresolved;
	_update_ui();
}

void PerforceDock::set_setup_state(int p_cache_files, bool p_p4v_ignores) {
	remove_cache_button->set_visible(p_cache_files > 0);
	p4ignore_button->set_visible(!p_p4v_ignores);
	if (p_cache_files > 0) {
		setup_label->set_text(vformat(TTR("The .godot folder (%d files) is in Perforce. It's the editor's local cache: it makes saving slow and causes errors for everyone. Remove it from the server; your local files are kept."), p_cache_files));
	} else if (!p_p4v_ignores) {
		setup_label->set_text(TTR("P4V doesn't use .p4ignore yet, so adding files in P4V can add the .godot cache folder. Godot already ignores it."));
	}
	setup_bar->set_visible(p_cache_files > 0 || !p_p4v_ignores);
}

void PerforceDock::add_log(const String &p_message, LogLevel p_level) {
	Color color;
	switch (p_level) {
		case LOG_INFO:
			color = get_theme_color(SceneStringName(font_color), EditorStringName(Editor));
			break;
		case LOG_SUCCESS:
			color = get_theme_color(SNAME("success_color"), EditorStringName(Editor));
			break;
		case LOG_WARNING:
			color = get_theme_color(SNAME("warning_color"), EditorStringName(Editor));
			break;
		case LOG_ERROR:
			color = get_theme_color(SNAME("error_color"), EditorStringName(Editor));
			break;
	}
	if (has_log) {
		log_text->add_newline();
	}
	has_log = true;
	log_text->push_color(get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor)));
	log_text->add_text(Time::get_singleton()->get_time_string_from_system() + "  ");
	log_text->pop();
	log_text->push_color(color);
	log_text->add_text(p_message);
	log_text->pop();
}

void PerforceDock::clear_description() {
	description_edit->clear();
	_update_ui();
}

void PerforceDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			get_latest_button->set_button_icon(get_editor_theme_icon(SNAME("MoveDown")));
			refresh_button->set_button_icon(get_editor_theme_icon(SNAME("Reload")));
			connection_button->set_button_icon(get_editor_theme_icon(SNAME("Tools")));
			resolve_button->set_button_icon(get_editor_theme_icon(SNAME("StatusWarning")));
			revert_button->set_button_icon(get_editor_theme_icon(SNAME("Close")));
			submit_button->set_button_icon(get_editor_theme_icon(SNAME("VCSCommit")));
			setup_icon->set_texture(get_editor_theme_icon(SNAME("StatusWarning")));
			setup_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("warning_color"), EditorStringName(Editor)));
			outdated_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("warning_color"), EditorStringName(Editor)));
			_update_ui();
		} break;
	}
}

void PerforceDock::_bind_methods() {
	ADD_SIGNAL(MethodInfo("refresh_requested"));
	ADD_SIGNAL(MethodInfo("connection_requested"));
	ADD_SIGNAL(MethodInfo("get_latest_requested"));
	ADD_SIGNAL(MethodInfo("resolve_requested"));
	ADD_SIGNAL(MethodInfo("remove_cache_requested"));
	ADD_SIGNAL(MethodInfo("enable_p4ignore_requested"));
	ADD_SIGNAL(MethodInfo("revert_requested", PropertyInfo(Variant::PACKED_STRING_ARRAY, "paths")));
	ADD_SIGNAL(MethodInfo("submit_requested", PropertyInfo(Variant::STRING, "description")));
}

PerforceDock::PerforceDock() {
	set_name("Perforce");
	set_title(TTR("Perforce"));
	set_icon_name("VcsBranches");
	set_default_slot(EditorDock::DOCK_SLOT_BOTTOM);
	set_available_layouts(EditorDock::DOCK_LAYOUT_HORIZONTAL | EditorDock::DOCK_LAYOUT_FLOATING);

	VBoxContainer *main_vb = memnew(VBoxContainer);
	main_vb->set_custom_minimum_size(Size2(0, 220) * EDSCALE);
	add_child(main_vb);

	// Status bar.
	HBoxContainer *top = memnew(HBoxContainer);
	main_vb->add_child(top);

	status_icon = memnew(TextureRect);
	status_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	top->add_child(status_icon);

	status_label = memnew(Label);
	status_label->set_h_size_flags(SIZE_EXPAND_FILL);
	status_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	status_label->set_text(TTR("Connecting to Perforce..."));
	top->add_child(status_label);

	outdated_label = memnew(Label);
	top->add_child(outdated_label);

	resolve_button = memnew(Button);
	resolve_button->set_tooltip_text(TTR("Merge the newer server versions into your checked out files."));
	resolve_button->connect(SceneStringName(pressed), callable_mp(this, &PerforceDock::_emit).bind(SNAME("resolve_requested")));
	top->add_child(resolve_button);

	get_latest_button = memnew(Button);
	get_latest_button->set_text(TTR("Get Latest"));
	get_latest_button->set_tooltip_text(TTR("Sync this project to the newest version on the server (p4 sync)."));
	get_latest_button->connect(SceneStringName(pressed), callable_mp(this, &PerforceDock::_emit).bind(SNAME("get_latest_requested")));
	top->add_child(get_latest_button);

	refresh_button = memnew(Button);
	refresh_button->set_flat(true);
	refresh_button->set_tooltip_text(TTR("Refresh"));
	refresh_button->connect(SceneStringName(pressed), callable_mp(this, &PerforceDock::_emit).bind(SNAME("refresh_requested")));
	top->add_child(refresh_button);

	connection_button = memnew(Button);
	connection_button->set_flat(true);
	connection_button->set_tooltip_text(TTR("Connection settings and login."));
	connection_button->connect(SceneStringName(pressed), callable_mp(this, &PerforceDock::_emit).bind(SNAME("connection_requested")));
	top->add_child(connection_button);

	setup_bar = memnew(HBoxContainer);
	setup_bar->hide();
	main_vb->add_child(setup_bar);

	setup_icon = memnew(TextureRect);
	setup_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	setup_bar->add_child(setup_icon);

	setup_label = memnew(Label);
	setup_label->set_h_size_flags(SIZE_EXPAND_FILL);
	setup_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	setup_bar->add_child(setup_label);

	remove_cache_button = memnew(Button);
	remove_cache_button->set_text(TTR("Remove .godot from Perforce"));
	remove_cache_button->connect(SceneStringName(pressed), callable_mp(this, &PerforceDock::_emit).bind(SNAME("remove_cache_requested")));
	setup_bar->add_child(remove_cache_button);

	p4ignore_button = memnew(Button);
	p4ignore_button->set_text(TTR("Make P4V Use .p4ignore"));
	p4ignore_button->set_tooltip_text(TTR("Runs \"p4 set P4IGNORE=.p4ignore\" for your user on this PC, so P4V also skips files listed in .p4ignore files."));
	p4ignore_button->connect(SceneStringName(pressed), callable_mp(this, &PerforceDock::_emit).bind(SNAME("enable_p4ignore_requested")));
	setup_bar->add_child(p4ignore_button);

	HSplitContainer *split = memnew(HSplitContainer);
	split->set_v_size_flags(SIZE_EXPAND_FILL);
	main_vb->add_child(split);

	// Checked out files.
	VBoxContainer *files_vb = memnew(VBoxContainer);
	files_vb->set_h_size_flags(SIZE_EXPAND_FILL);
	files_vb->set_stretch_ratio(1.5);
	split->add_child(files_vb);

	files_label = memnew(Label);
	files_vb->add_child(files_label);

	files_tree = memnew(Tree);
	files_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	files_tree->set_hide_root(true);
	files_tree->set_select_mode(Tree::SELECT_MULTI);
	files_tree->set_columns(2);
	files_tree->set_column_expand(0, false);
	files_tree->set_column_custom_minimum_width(0, 100 * EDSCALE);
	files_tree->set_column_clip_content(1, true);
	files_tree->set_tooltip_text(TTR("Double-click to show the file in the FileSystem dock."));
	files_tree->connect("multi_selected", callable_mp(this, &PerforceDock::_files_selected).unbind(3));
	files_tree->connect("item_activated", callable_mp(this, &PerforceDock::_files_activated));
	files_vb->add_child(files_tree);

	HBoxContainer *submit_hb = memnew(HBoxContainer);
	files_vb->add_child(submit_hb);

	revert_button = memnew(Button);
	revert_button->set_text(TTR("Revert"));
	revert_button->set_tooltip_text(TTR("Discard your changes to the selected files."));
	revert_button->connect(SceneStringName(pressed), callable_mp(this, &PerforceDock::_revert_pressed));
	submit_hb->add_child(revert_button);

	description_edit = memnew(LineEdit);
	description_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	description_edit->set_placeholder(TTR("Describe your changes..."));
	description_edit->connect(SceneStringName(text_changed), callable_mp(this, &PerforceDock::_description_changed));
	description_edit->connect(SceneStringName(text_submitted), callable_mp(this, &PerforceDock::_submit_pressed).unbind(1));
	submit_hb->add_child(description_edit);

	submit_button = memnew(Button);
	submit_button->set_text(TTR("Submit"));
	submit_button->connect(SceneStringName(pressed), callable_mp(this, &PerforceDock::_submit_pressed));
	submit_hb->add_child(submit_button);

	// Activity log.
	VBoxContainer *log_vb = memnew(VBoxContainer);
	log_vb->set_h_size_flags(SIZE_EXPAND_FILL);
	split->add_child(log_vb);

	Label *log_label = memnew(Label);
	log_label->set_text(TTR("Activity:"));
	log_vb->add_child(log_label);

	log_text = memnew(RichTextLabel);
	log_text->set_v_size_flags(SIZE_EXPAND_FILL);
	log_text->set_scroll_follow(true);
	log_text->set_selection_enabled(true);
	log_text->set_context_menu_enabled(true);
	log_text->set_focus_mode(FOCUS_CLICK);
	log_vb->add_child(log_text);
}
