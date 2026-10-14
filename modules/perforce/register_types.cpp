/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "perforce_context_menu.h"
#include "perforce_dock.h"
#include "perforce_plugin.h"
#include "perforce_resolve_dialog.h"
#include "scene_merger.h"

#include "core/object/class_db.h"
#include "core/os/os.h"

#include <cstdio>
#include <cstdlib>

void initialize_perforce_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_CORE) {
		// `godot --headless --merge-scene <base> <theirs> <yours> <result>` runs the
		// scene merge tool (for P4V / `p4 resolve`) and exits before anything else starts.
		List<String> cmdline = OS::get_singleton()->get_cmdline_args();
		if (cmdline.find("--merge-scene")) {
			Vector<String> args;
			for (const String &arg : cmdline) {
				args.push_back(arg);
			}
			int exit_code = SceneMerger::run_command_line(args);
			// Skip engine shutdown (nothing else was started) to avoid leak reports at exit.
			fflush(stdout);
			fflush(stderr);
			std::_Exit(exit_code);
		}
	} else if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(PerforceContextMenu);
		GDREGISTER_INTERNAL_CLASS(PerforceDock);
		GDREGISTER_INTERNAL_CLASS(PerforceResolveDialog);
		GDREGISTER_INTERNAL_CLASS(PerforcePlugin);
		EditorPlugins::add_by_type<PerforcePlugin>();
	}
}

void uninitialize_perforce_module(ModuleInitializationLevel p_level) {
}
