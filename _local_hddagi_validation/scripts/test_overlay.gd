extends CanvasLayer

## Draws the current validation-case title and notes.

## Short case identifier shown in the overlay header.
@export var case_id: String = "00"
## Human-readable case title.
@export var case_title: String = "Global HDDAGI baseline"
## Extra inspection notes shown under the title.
@export var case_notes: String = "Global HDDAGI only. Confirm color bleeding before any Local work."

@onready var _label: Label = $Label

var _active_view: String = ""


func _ready() -> void:
	_refresh_label()


## Updates the current view name, for example Local vs Global interior.
func set_active_view(view_name: String) -> void:
	_active_view = view_name
	_refresh_label()


func _refresh_label() -> void:
	var text := "%s — %s\n%s" % [case_id, case_title, case_notes]
	if not _active_view.is_empty():
		text += "\nView: %s  (WASD move, Q/E up-down, hold RMB look, Tab switch)" % _active_view
	_label.text = text
