/**************************************************************************/
/*  perforce_resolve_dialog.cpp                                           */
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

#include "perforce_resolve_dialog.h"

#include "core/object/callable_mp.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/tree.h"

enum {
	COLUMN_WHERE,
	COLUMN_YOURS,
	COLUMN_THEIRS,
};

static String _short(const String &p_value, bool p_present) {
	if (!p_present) {
		return TTR("(removed)");
	}
	String value = p_value.strip_edges();
	if (value.length() > 200) {
		value = value.left(200) + "...";
	}
	return value;
}

void PerforceResolveDialog::open(const String &p_file, const Vector<SceneMerger::Conflict> &p_conflicts) {
	file = p_file;
	header->set_text(vformat(TTRN("%s was changed by you and on the server. Everything else merged automatically; pick which version to keep for this %d change:", "%s was changed by you and on the server. Everything else merged automatically; pick which version to keep for these %d changes:", p_conflicts.size()), p_file.get_file(), p_conflicts.size()));

	tree->clear();
	TreeItem *root = tree->create_item();
	for (const SceneMerger::Conflict &conflict : p_conflicts) {
		TreeItem *item = tree->create_item(root);
		item->set_text(COLUMN_WHERE, conflict.property.is_empty() ? conflict.section : conflict.section + " > " + conflict.property);
		item->set_tooltip_text(COLUMN_WHERE, TTR("Original:") + " " + _short(conflict.base, conflict.in_base));
		item->set_metadata(COLUMN_WHERE, conflict.id);

		for (int column : { COLUMN_YOURS, COLUMN_THEIRS }) {
			const bool yours = column == COLUMN_YOURS;
			item->set_cell_mode(column, TreeItem::CELL_MODE_CHECK);
			item->set_editable(column, true);
			item->set_text(column, _short(yours ? conflict.ours : conflict.theirs, yours ? conflict.in_ours : conflict.in_theirs));
			item->set_tooltip_text(column, yours ? conflict.ours : conflict.theirs);
			item->set_checked(column, yours);
		}
	}
	popup_centered_clamped(Size2(900, 450) * EDSCALE, 0.8);
}

// The two checkboxes of a row act like radio buttons.
void PerforceResolveDialog::_item_edited() {
	TreeItem *item = tree->get_edited();
	int column = tree->get_edited_column();
	if (item && (column == COLUMN_YOURS || column == COLUMN_THEIRS)) {
		item->set_checked(column, true);
		item->set_checked(column == COLUMN_YOURS ? COLUMN_THEIRS : COLUMN_YOURS, false);
	}
}

void PerforceResolveDialog::_use_all(bool p_theirs) {
	for (TreeItem *item = tree->get_root()->get_first_child(); item; item = item->get_next()) {
		item->set_checked(COLUMN_YOURS, !p_theirs);
		item->set_checked(COLUMN_THEIRS, p_theirs);
	}
}

void PerforceResolveDialog::_confirmed() {
	Dictionary use_theirs;
	for (TreeItem *item = tree->get_root()->get_first_child(); item; item = item->get_next()) {
		use_theirs[item->get_metadata(COLUMN_WHERE)] = item->is_checked(COLUMN_THEIRS);
	}
	emit_signal(SNAME("resolved"), file, use_theirs);
}

void PerforceResolveDialog::_bind_methods() {
	ADD_SIGNAL(MethodInfo("resolved", PropertyInfo(Variant::STRING, "file"), PropertyInfo(Variant::DICTIONARY, "use_theirs")));
}

PerforceResolveDialog::PerforceResolveDialog() {
	set_title(TTR("Merge Conflicts"));
	set_ok_button_text(TTR("Merge"));
	set_cancel_button_text(TTR("Later"));
	connect(SceneStringName(confirmed), callable_mp(this, &PerforceResolveDialog::_confirmed));

	VBoxContainer *vb = memnew(VBoxContainer);
	add_child(vb);

	header = memnew(Label);
	header->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	header->set_custom_minimum_size(Size2(600, 0) * EDSCALE);
	vb->add_child(header);

	tree = memnew(Tree);
	tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tree->set_hide_root(true);
	tree->set_columns(3);
	tree->set_column_titles_visible(true);
	tree->set_column_title(COLUMN_WHERE, TTR("Changed"));
	tree->set_column_title(COLUMN_YOURS, TTR("Yours"));
	tree->set_column_title(COLUMN_THEIRS, TTR("Server"));
	tree->set_column_expand_ratio(COLUMN_WHERE, 2);
	tree->set_column_clip_content(COLUMN_YOURS, true);
	tree->set_column_clip_content(COLUMN_THEIRS, true);
	// The two checkboxes of a row act like radio buttons.
	tree->connect("item_edited", callable_mp(this, &PerforceResolveDialog::_item_edited));
	vb->add_child(tree);

	HBoxContainer *hb = memnew(HBoxContainer);
	vb->add_child(hb);
	Button *all_yours = memnew(Button(TTR("Keep All Yours")));
	all_yours->connect(SceneStringName(pressed), callable_mp(this, &PerforceResolveDialog::_use_all).bind(false));
	hb->add_child(all_yours);
	Button *all_theirs = memnew(Button(TTR("Take All From Server")));
	all_theirs->connect(SceneStringName(pressed), callable_mp(this, &PerforceResolveDialog::_use_all).bind(true));
	hb->add_child(all_theirs);
}
