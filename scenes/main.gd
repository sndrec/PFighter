extends Node2D


@onready var connection_panel = $CanvasLayer/ConnectionPanel
@onready var host_field = $CanvasLayer/ConnectionPanel/GridContainer/HostField
@onready var port_field = $CanvasLayer/ConnectionPanel/GridContainer/PortField
@onready var message_label = $CanvasLayer/MessageLabel
@onready var sync_lost_label = $CanvasLayer/SyncLostLabel
@onready var stage_camera = $SubViewportContainer/SubViewport/StageCamera

const BattlefieldStage = preload("res://scenes/stage/battlefield.tscn")
const RookFighter = preload("res://scenes/char/rook/ft_rook.tscn")

const LOG_FILE_DIRECTORY = "user://detailed_logs"

var logging_enabled := false

var curStage

func _ready() -> void:
	get_tree().get_multiplayer().connect("peer_connected", Callable(self, "_on_multiplayer_peer_connected"))
	get_tree().get_multiplayer().connect("peer_disconnected", Callable(self, "_on_multiplayer_peer_disconnected"))
	get_tree().get_multiplayer().connect("server_disconnected", Callable(self, "_on_server_disconnected"))
	SyncManager.connect("sync_started", Callable(self, "_on_SyncManager_sync_started"))
	SyncManager.connect("sync_stopped", Callable(self, "_on_SyncManager_sync_stopped"))
	SyncManager.connect("sync_lost", Callable(self, "_on_SyncManager_sync_lost"))
	SyncManager.connect("sync_regained", Callable(self, "_on_SyncManager_sync_regained"))
	SyncManager.connect("sync_error", Callable(self, "_on_SyncManager_sync_error"))

func _on_ServerButton_pressed():
	var peer = ENetMultiplayerPeer.new()
	peer.create_server(int(port_field.text), 1)
	get_tree().get_multiplayer().multiplayer_peer = peer
	connection_panel.visible = false
	message_label.text = "Listening..."

func _on_ClientButton_pressed():
	var peer = ENetMultiplayerPeer.new()
	peer.create_client(host_field.text, int(port_field.text))
	get_tree().get_multiplayer().multiplayer_peer = peer
	connection_panel.visible = false
	message_label.text = "Connecting..."

func _on_multiplayer_peer_connected(peer_id: int):
	message_label.text = "Connected!"
	SyncManager.add_peer(peer_id) 
	
	$ServerPlayer.set_multiplayer_authority(1)
	
	if get_tree().get_multiplayer().is_server():
		$ClientPlayer.set_multiplayer_authority(peer_id)
	else:
		$ClientPlayer.set_multiplayer_authority(get_tree().get_multiplayer().get_unique_id())

	if get_tree().get_multiplayer().is_server():
		message_label.text = "Starting..."
		await get_tree().create_timer(2.0).timeout
		SyncManager.start()

func _on_multiplayer_peer_disconnected(peer_id: int):
	message_label.text = "Disconnected."
	SyncManager.remove_peer(peer_id)

func _on_server_disconnected() -> void:
	_on_multiplayer_peer_disconnected(1)


func _on_ResetButton_pressed():
	SyncManager.stop()
	SyncManager.clear_peers()
	var peer = get_tree().get_multiplayer().multiplayer_peer
	if peer:
		peer.close()
	get_tree().reload_current_scene()

func _on_SyncManager_sync_started() -> void:
	message_label.text = "Started!"
	
	if logging_enabled:
		if not DirAccess.dir_exists_absolute(LOG_FILE_DIRECTORY):
			DirAccess.make_dir_absolute(LOG_FILE_DIRECTORY)
		
		var datetime = Time.get_datetime_dict_from_system(true)
		var log_file_name = "%04d%02d%02d-%02d%02d%02d-peer-%d.log" % [
			datetime['year'],
			datetime['month'],
			datetime['day'],
			datetime['hour'],
			datetime['minute'],
			datetime['second'],
			get_tree().get_multiplayer().get_unique_id()
		]
		
		SyncManager.start_logging(LOG_FILE_DIRECTORY + '/' + log_file_name)

	var Stage = SyncManager.spawn("Stage", $StageContainer, BattlefieldStage, {})
	var Fighter1 = SyncManager.spawn("Fighter1", $StageContainer, RookFighter, {"position" = Vector3(0, 0, 0), "controller" = $ServerPlayer})
	var Fighter2 = SyncManager.spawn("Fighter2", $StageContainer, RookFighter, {"position" = Vector3(0, 0, 0), "controller" = $ClientPlayer})
	Fighter1.stage = Stage
	Fighter2.stage = Stage
	Fighter1.fighterID = 0
	Fighter2.fighterID = 1
	Fighter1.badgeGrid = %BadgeGrid
	Fighter2.badgeGrid = %BadgeGrid
	$ServerPlayer.controlledFighter = Fighter1
	$ClientPlayer.controlledFighter = Fighter2
	Stage.fighters.append(Fighter1)
	Stage.fighters.append(Fighter2)
	curStage = Stage
	%BadgeGrid.stage = Stage
	%BadgeGrid.update_player_percent(Fighter1)
	%BadgeGrid.update_player_percent(Fighter2)

func _process(delta: float) -> void:
	if curStage and curStage != null and curStage.has_node("CameraController"):
		var cameraController = curStage.get_node("CameraController")
		stage_camera.transform = curStage.cameraTransform

func _on_SyncManager_sync_stopped() -> void:
	if logging_enabled:
		SyncManager.stop_logging()

func _on_SyncManager_sync_lost() -> void:
	sync_lost_label.visible = true

func _on_SyncManager_sync_regained() -> void:
	sync_lost_label.visible = false

func _on_SyncManager_sync_error(msg: String) -> void:
	message_label.text = "Fatal sync error: " + msg
	sync_lost_label.visible = false
	
	var peer = get_tree().get_multiplayer().multiplayer_peer
	if peer:
		peer.close()
	SyncManager.clear_peers()
