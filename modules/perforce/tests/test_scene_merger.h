/**************************************************************************/
/*  test_scene_merger.h                                                   */
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

#include "../scene_merger.h"

#include "tests/test_macros.h"

namespace TestSceneMerger {

// A small scene as Godot 4.7 saves it.
static const char *BASE_SCENE = R"([gd_scene format=3 uid="uid://b1"]

[ext_resource type="Script" uid="uid://s1" path="res://player.gd" id="1_abcde"]

[sub_resource type="RectangleShape2D" id="RectangleShape2D_aaaaa"]
size = Vector2(10, 10)

[node name="Level" type="Node2D" unique_id=100]

[node name="Player" type="CharacterBody2D" parent="." unique_id=200]
script = ExtResource("1_abcde")
speed = 100

[node name="Shape" type="CollisionShape2D" parent="Player" unique_id=300]
shape = SubResource("RectangleShape2D_aaaaa")

[node name="Enemy" type="Node2D" parent="." unique_id=400]
position = Vector2(5, 5)
)";

static String replace(const String &p_text, const String &p_from, const String &p_to) {
	CHECK_MESSAGE(p_text.contains(p_from), (String("Test fixture is missing: ") + p_from));
	return p_text.replace(p_from, p_to);
}

TEST_CASE("[SceneMerger] Unchanged inputs round-trip exactly") {
	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, BASE_SCENE, BASE_SCENE);
	CHECK(result.error.is_empty());
	CHECK(result.conflicts.is_empty());
	CHECK(result.text == String(BASE_SCENE));
}

TEST_CASE("[SceneMerger] Edits to different nodes merge cleanly") {
	const String ours = replace(BASE_SCENE, "speed = 100", "speed = 250");
	const String theirs = replace(BASE_SCENE, "position = Vector2(5, 5)", "position = Vector2(50, 5)");

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	CHECK(result.conflicts.is_empty());
	CHECK(result.text.contains("speed = 250"));
	CHECK(result.text.contains("position = Vector2(50, 5)"));
}

TEST_CASE("[SceneMerger] Different properties of the same node merge cleanly") {
	const String ours = replace(BASE_SCENE, "speed = 100", "speed = 250");
	const String theirs = replace(BASE_SCENE, "speed = 100", "speed = 100\nvisible = false");

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	CHECK(result.conflicts.is_empty());
	CHECK(result.text.contains("speed = 250"));
	CHECK(result.text.contains("visible = false"));
}

TEST_CASE("[SceneMerger] Nodes added on both sides are all kept") {
	const String ours = String(BASE_SCENE) + "\n[node name=\"Coin\" type=\"Area2D\" parent=\".\" unique_id=500]\n";
	const String theirs = String(BASE_SCENE) + "\n[node name=\"Door\" type=\"Area2D\" parent=\".\" unique_id=600]\n";

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	CHECK(result.conflicts.is_empty());
	CHECK(result.text.contains("[node name=\"Coin\""));
	CHECK(result.text.contains("[node name=\"Door\""));
}

TEST_CASE("[SceneMerger] Same property changed differently is a conflict") {
	const String ours = replace(BASE_SCENE, "speed = 100", "speed = 250");
	const String theirs = replace(BASE_SCENE, "speed = 100", "speed = 300");

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	REQUIRE(result.conflicts.size() == 1);
	const SceneMerger::Conflict &conflict = result.conflicts[0];
	CHECK(conflict.section == "Node Player");
	CHECK(conflict.property == "speed");
	CHECK(conflict.base == "100");
	CHECK(conflict.ours == "250");
	CHECK(conflict.theirs == "300");
	CHECK(result.text.contains("speed = 250")); // Ours by default.

	HashMap<String, SceneMerger::Choice> choices;
	choices[conflict.id] = SceneMerger::CHOICE_THEIRS;
	SceneMerger::Result chosen = SceneMerger::merge(BASE_SCENE, ours, theirs, choices);
	CHECK(chosen.text.contains("speed = 300"));
	CHECK_FALSE(chosen.text.contains("speed = 250"));
}

TEST_CASE("[SceneMerger] Colliding new sub-resource ids are renamed") {
	// Both sides add a sub-resource with the same (path-seeded) id but different contents.
	const String ours = replace(BASE_SCENE, "[node name=\"Level\"",
									  "[sub_resource type=\"CircleShape2D\" id=\"CircleShape2D_zzzzz\"]\nradius = 1.0\n\n[node name=\"Level\"") +
			"\n[node name=\"Coin\" type=\"CollisionShape2D\" parent=\".\" unique_id=500]\nshape = SubResource(\"CircleShape2D_zzzzz\")\n";
	const String theirs = replace(BASE_SCENE, "[node name=\"Level\"",
									"[sub_resource type=\"CircleShape2D\" id=\"CircleShape2D_zzzzz\"]\nradius = 9.0\n\n[node name=\"Level\"") +
			"\n[node name=\"Door\" type=\"CollisionShape2D\" parent=\".\" unique_id=600]\nshape = SubResource(\"CircleShape2D_zzzzz\")\n";

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	CHECK(result.conflicts.is_empty());
	CHECK(result.text.contains("radius = 1.0"));
	CHECK(result.text.contains("radius = 9.0"));
	CHECK(result.text.contains("[sub_resource type=\"CircleShape2D\" id=\"CircleShape2D_zzzzz_m1\"]"));
	// Each node still points at its own shape.
	CHECK(result.text.contains("[node name=\"Coin\" type=\"CollisionShape2D\" parent=\".\" unique_id=500]\nshape = SubResource(\"CircleShape2D_zzzzz\")"));
	CHECK(result.text.contains("[node name=\"Door\" type=\"CollisionShape2D\" parent=\".\" unique_id=600]\nshape = SubResource(\"CircleShape2D_zzzzz_m1\")"));
}

TEST_CASE("[SceneMerger] The same external resource added on both sides is kept once") {
	const String ours = replace(BASE_SCENE, "id=\"1_abcde\"]\n", "id=\"1_abcde\"]\n[ext_resource type=\"Texture2D\" uid=\"uid://t1\" path=\"res://coin.png\" id=\"2_ours1\"]\n") +
			"\n[node name=\"Coin\" type=\"Sprite2D\" parent=\".\" unique_id=500]\ntexture = ExtResource(\"2_ours1\")\n";
	const String theirs = replace(BASE_SCENE, "id=\"1_abcde\"]\n", "id=\"1_abcde\"]\n[ext_resource type=\"Texture2D\" uid=\"uid://t1\" path=\"res://coin.png\" id=\"2_thei1\"]\n") +
			"\n[node name=\"Door\" type=\"Sprite2D\" parent=\".\" unique_id=600]\ntexture = ExtResource(\"2_thei1\")\n";

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	CHECK(result.conflicts.is_empty());
	CHECK(result.text.count("path=\"res://coin.png\"") == 1);
	CHECK_FALSE(result.text.contains("2_thei1"));
	CHECK(result.text.count("texture = ExtResource(\"2_ours1\")") == 2);
}

TEST_CASE("[SceneMerger] Different external resources with the same id are renumbered") {
	const String ours = replace(BASE_SCENE, "id=\"1_abcde\"]\n", "id=\"1_abcde\"]\n[ext_resource type=\"Texture2D\" uid=\"uid://t1\" path=\"res://coin.png\" id=\"2_same1\"]\n");
	const String theirs = replace(BASE_SCENE, "id=\"1_abcde\"]\n", "id=\"1_abcde\"]\n[ext_resource type=\"Texture2D\" uid=\"uid://t2\" path=\"res://door.png\" id=\"2_same1\"]\n") +
			"\n[node name=\"Door\" type=\"Sprite2D\" parent=\".\" unique_id=600]\ntexture = ExtResource(\"2_same1\")\n";

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	CHECK(result.conflicts.is_empty());
	CHECK(result.text.contains("path=\"res://coin.png\" id=\"2_same1\""));
	CHECK(result.text.contains("path=\"res://door.png\" id=\"2_same1_m1\""));
	CHECK(result.text.contains("texture = ExtResource(\"2_same1_m1\")"));
}

TEST_CASE("[SceneMerger] Renamed nodes keep matching and children follow") {
	// We rename Player to Hero; they add a child under Player and edit Player.
	const String ours = replace(replace(BASE_SCENE, "[node name=\"Player\"", "[node name=\"Hero\""), "parent=\"Player\"", "parent=\"Hero\"");
	const String theirs = replace(BASE_SCENE, "speed = 100", "speed = 300") +
			"\n[node name=\"Hat\" type=\"Sprite2D\" parent=\"Player\" unique_id=700]\n";

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	CHECK(result.conflicts.is_empty());
	CHECK(result.text.contains("[node name=\"Hero\" type=\"CharacterBody2D\" parent=\".\" unique_id=200]"));
	CHECK(result.text.contains("speed = 300"));
	CHECK(result.text.contains("[node name=\"Hat\" type=\"Sprite2D\" parent=\"Hero\" unique_id=700]"));
	CHECK_FALSE(result.text.contains("\"Player\""));
}

TEST_CASE("[SceneMerger] Deleting a node the other side edited is a conflict") {
	const String ours = replace(BASE_SCENE, "\n[node name=\"Enemy\" type=\"Node2D\" parent=\".\" unique_id=400]\nposition = Vector2(5, 5)\n", "");
	const String theirs = replace(BASE_SCENE, "position = Vector2(5, 5)", "position = Vector2(9, 9)");

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	REQUIRE(result.conflicts.size() == 1);
	CHECK(result.conflicts[0].section == "Node Enemy");
	CHECK(result.conflicts[0].property.is_empty());
	CHECK_FALSE(result.text.contains("Enemy")); // Ours (deleted) by default.

	HashMap<String, SceneMerger::Choice> choices;
	choices[result.conflicts[0].id] = SceneMerger::CHOICE_THEIRS;
	CHECK(SceneMerger::merge(BASE_SCENE, ours, theirs, choices).text.contains("position = Vector2(9, 9)"));
}

TEST_CASE("[SceneMerger] Deleting an untouched node merges cleanly") {
	const String theirs = replace(BASE_SCENE, "\n[node name=\"Enemy\" type=\"Node2D\" parent=\".\" unique_id=400]\nposition = Vector2(5, 5)\n", "");
	const String ours = replace(BASE_SCENE, "speed = 100", "speed = 250");

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	CHECK(result.conflicts.is_empty());
	CHECK_FALSE(result.text.contains("Enemy"));
	CHECK(result.text.contains("speed = 250"));
}

TEST_CASE("[SceneMerger] Connections from both sides are kept") {
	const String ours = String(BASE_SCENE) + "\n[connection signal=\"ready\" from=\"Player\" to=\".\" method=\"_on_player_ready\"]\n";
	const String theirs = String(BASE_SCENE) + "\n[connection signal=\"ready\" from=\"Enemy\" to=\".\" method=\"_on_enemy_ready\"]\n";

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	CHECK(result.conflicts.is_empty());
	// Connections are written back to back after the nodes, like Godot does.
	CHECK(result.text.ends_with("[connection signal=\"ready\" from=\"Player\" to=\".\" method=\"_on_player_ready\"]\n[connection signal=\"ready\" from=\"Enemy\" to=\".\" method=\"_on_enemy_ready\"]\n"));
}

TEST_CASE("[SceneMerger] Multi-line values and strings are preserved") {
	const String ours = replace(BASE_SCENE, "speed = 100", "speed = 100\ntext = \"line one\nline [two] = 2\"\npoints = PackedVector2Array(0, 0,\n1, 1)");
	const String theirs = replace(BASE_SCENE, "position = Vector2(5, 5)", "position = Vector2(6, 6)");

	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, ours, theirs);
	CHECK(result.error.is_empty());
	CHECK(result.conflicts.is_empty());
	CHECK(result.text.contains("text = \"line one\nline [two] = 2\""));
	CHECK(result.text.contains("points = PackedVector2Array(0, 0,\n1, 1)"));
	CHECK(result.text.contains("position = Vector2(6, 6)"));
}

TEST_CASE("[SceneMerger] Garbage input reports an error") {
	SceneMerger::Result result = SceneMerger::merge(BASE_SCENE, "not a scene", BASE_SCENE);
	CHECK_FALSE(result.error.is_empty());
	CHECK(result.text.is_empty());
}

// project.godot as Godot 4.7 writes it.
static const char *BASE_PROJECT = R"(; Engine configuration file.
; It's best edited using the editor UI and not directly,
; since the parameters that go here are not all obvious.
;
; Format:
;   [section] ; section goes between []
;   param=value ; assign values to parameters

config_version=5

[application]

config/name="Game"
config/features=PackedStringArray("4.7", "Forward Plus")

[input]

move_left={
"deadzone": 0.2,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":0,"physical_keycode":65,"key_label":0,"unicode":97,"location":0,"echo":false,"script":null)
]
}
)";

static String add_action(const String &p_project, const String &p_name, int p_key) {
	return p_project + vformat("%s={\n\"deadzone\": 0.2,\n\"events\": [Object(InputEventKey,\"physical_keycode\":%d,\"script\":null)\n]\n}\n", p_name, p_key);
}

TEST_CASE("[SceneMerger] project.godot round-trips exactly") {
	SceneMerger::Result result = SceneMerger::merge(BASE_PROJECT, BASE_PROJECT, BASE_PROJECT);
	CHECK(result.error.is_empty());
	CHECK(result.conflicts.is_empty());
	CHECK(result.text == String(BASE_PROJECT));
}

TEST_CASE("[SceneMerger] Input actions added by two people are both kept") {
	const String ours = add_action(BASE_PROJECT, "jump", 32);
	const String theirs = add_action(BASE_PROJECT, "crouch", 67);

	SceneMerger::Result result = SceneMerger::merge(BASE_PROJECT, ours, theirs);
	CHECK(result.error.is_empty());
	CHECK(result.conflicts.is_empty());
	CHECK(result.text.contains("\njump={"));
	CHECK(result.text.contains("\ncrouch={"));
	CHECK(result.text.contains("\nmove_left={"));
	CHECK(result.text.begins_with("; Engine configuration file."));
	CHECK(result.text.contains("\nconfig_version=5\n"));
}

TEST_CASE("[SceneMerger] New sections and settings from both sides are kept") {
	const String ours = String(BASE_PROJECT) + "\n[autoload]\n\nGame=\"*res://game.gd\"\n";
	const String theirs = String(BASE_PROJECT).replace("config/name=\"Game\"", "config/name=\"Game\"\nrun/main_scene=\"res://main.tscn\"");

	SceneMerger::Result result = SceneMerger::merge(BASE_PROJECT, ours, theirs);
	CHECK(result.conflicts.is_empty());
	CHECK(result.text.contains("[autoload]\n\nGame=\"*res://game.gd\"\n"));
	CHECK(result.text.contains("config/name=\"Game\"\nrun/main_scene=\"res://main.tscn\"\n"));
}

TEST_CASE("[SceneMerger] The same setting changed differently in project.godot is a conflict") {
	const String ours = String(BASE_PROJECT).replace("config/name=\"Game\"", "config/name=\"Ours\"");
	const String theirs = String(BASE_PROJECT).replace("config/name=\"Game\"", "config/name=\"Theirs\"");

	SceneMerger::Result result = SceneMerger::merge(BASE_PROJECT, ours, theirs);
	REQUIRE(result.conflicts.size() == 1);
	CHECK(result.conflicts[0].section == "[application]");
	CHECK(result.conflicts[0].property == "config/name");
	CHECK(result.text.contains("config/name=\"Ours\""));
}

} // namespace TestSceneMerger
