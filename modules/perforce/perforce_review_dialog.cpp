/**************************************************************************/
/*  perforce_review_dialog.cpp                                            */
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

#include "perforce_review_dialog.h"

#include "core/object/callable_mp.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/tree.h"

static const char *ACTION_CHECKOUT = "checkout";
static const char *ACTION_ADD = "add";
static const char *ACTION_RESTORE = "restore";
static const char *ACTION_DELETE = "delete";

void PerforceReviewDialog::open(const String &p_project_root, const PackedStringArray &p_changed, const PackedStringArray &p_new, const PackedStringArray &p_missing) {
	project_root = p_project_root.trim_suffix("/") + "/";
	tree->clear();
	TreeItem *root = tree->create_item();

	struct Group {
		Category category;
		String title;
		const PackedStringArray &paths;
		bool checked;
	};
	// Missing files start unchecked: deciding what happens to them is deliberate.
	const Group groups[CATEGORY_MAX] = {
		{ CATEGORY_CHANGED, TTR("Changed on disk but not checked out (changes would not be submitted)"), p_changed, true },
		{ CATEGORY_NEW, TTR("New files that aren't in Perforce yet"), p_new, true },
		{ CATEGORY_MISSING, TTR("Missing on disk, but Perforce still has them"), p_missing, false },
	};
	for (const Group &group : groups) {
		TreeItem *category = tree->create_item(root);
		category->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
		category->set_editable(0, true);
		category->set_checked(0, group.checked && !group.paths.is_empty());
		category->set_text(0, vformat("%s (%d)", group.title, group.paths.size()));
		category->set_selectable(0, false);
		category->set_collapsed(group.paths.size() > 30);
		categories[group.category] = category;
		for (const String &path : group.paths) {
			TreeItem *item = tree->create_item(category);
			item->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
			item->set_editable(0, true);
			item->set_checked(0, group.checked);
			const String relative = path.replace_char('\\', '/');
			item->set_text(0, relative.to_lower().begins_with(project_root.to_lower()) ? relative.substr(project_root.length()) : relative);
			item->set_tooltip_text(0, path);
			item->set_metadata(0, path);
		}
		if (group.paths.is_empty()) {
			category->set_custom_color(0, Color(1, 1, 1, 0.5));
		}
	}

	const int total = p_changed.size() + p_new.size() + p_missing.size();
	header->set_text(total == 0 ? TTR("Your workspace matches Perforce. Nothing to review.") : TTR("Tick the files to act on, then pick what to do. Nothing happens on its own: missing files are never marked for delete unless you choose \"Mark for Delete\"."));
	popup_centered_clamped(Size2(900, 560) * EDSCALE, 0.85);
}

// Ticking a category ticks all of its files.
void PerforceReviewDialog::_item_edited() {
	TreeItem *item = tree->get_edited();
	if (!item) {
		return;
	}
	for (TreeItem *category : categories) {
		if (item == category) {
			for (TreeItem *child = category->get_first_child(); child; child = child->get_next()) {
				child->set_checked(0, category->is_checked(0));
			}
		}
	}
}

PackedStringArray PerforceReviewDialog::_checked(Category p_category) const {
	PackedStringArray paths;
	for (TreeItem *child = categories[p_category]->get_first_child(); child; child = child->get_next()) {
		if (child->is_checked(0)) {
			paths.push_back(child->get_metadata(0));
		}
	}
	return paths;
}

void PerforceReviewDialog::_custom_action(const StringName &p_action) {
	const String action = p_action;
	Category category = CATEGORY_MISSING;
	if (action == ACTION_CHECKOUT) {
		category = CATEGORY_CHANGED;
	} else if (action == ACTION_ADD) {
		category = CATEGORY_NEW;
	}
	const PackedStringArray paths = _checked(category);
	if (paths.is_empty()) {
		return;
	}
	hide();
	emit_signal(SNAME("apply_requested"), action, paths);
}

void PerforceReviewDialog::_bind_methods() {
	ADD_SIGNAL(MethodInfo("apply_requested", PropertyInfo(Variant::STRING, "action"), PropertyInfo(Variant::PACKED_STRING_ARRAY, "paths")));
}

PerforceReviewDialog::PerforceReviewDialog() {
	set_title(TTR("Review Workspace"));
	set_ok_button_text(TTR("Close"));

	VBoxContainer *vb = memnew(VBoxContainer);
	add_child(vb);

	header = memnew(Label);
	header->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	header->set_custom_minimum_size(Size2(600, 0) * EDSCALE);
	vb->add_child(header);

	tree = memnew(Tree);
	tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tree->set_hide_root(true);
	tree->connect("item_edited", callable_mp(this, &PerforceReviewDialog::_item_edited));
	vb->add_child(tree);

	add_button(TTR("Check Out Ticked Changed Files"), false, ACTION_CHECKOUT);
	add_button(TTR("Add Ticked New Files"), false, ACTION_ADD);
	add_button(TTR("Restore Ticked Missing Files"), false, ACTION_RESTORE);
	add_button(TTR("Mark Ticked Missing Files for Delete..."), false, ACTION_DELETE);
	connect("custom_action", callable_mp(this, &PerforceReviewDialog::_custom_action));
}
