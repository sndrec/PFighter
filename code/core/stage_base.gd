@tool

extends Node

class_name PFStage

@export var StageCollisionSegments: Array[StageCollisionSegment] = []

@export var StageLedgePoints: Array[StageLedgePoint] = []

@export var StageBounds: Array[Vector2] = [Vector2.ZERO, Vector2.ZERO]

@export var CameraBounds: Array[Vector2] = [Vector2.ZERO, Vector2.ZERO]

var fighters: Array[Fighter] = []

var cameraTransform: Transform3D = Transform3D.IDENTITY

func _ready():
	pass # Replace with function body.

# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta):
	if !Engine.is_editor_hint():
		return
	for seg in StageCollisionSegments:
		var start3D = Vector3(seg.startOffset.x, seg.startOffset.y, 0)
		var end3D = Vector3(seg.endOffset.x, seg.endOffset.y, 0)
		var midPoint = (start3D + end3D) * 0.5
		var dir = (end3D - start3D).normalized().rotated(Vector3(0, 0, 1), PI * 0.5)
		var lineColor = Color(1,1,1)
		if seg.colType == seg.collisionType.semisolid:
			lineColor = Color(0.2, 0.2, 1)
		DebugDraw.draw_line(start3D, end3D, lineColor, 0.016)
		DebugDraw.draw_arrow_line(midPoint, midPoint + dir * 8, lineColor, 1, true, 0.016)
		
		var topLeft = Vector3(CameraBounds[0].x, CameraBounds[0].y, 0)
		var topRight = Vector3(CameraBounds[1].x, CameraBounds[0].y, 0)
		var botRight = Vector3(CameraBounds[1].x, CameraBounds[1].y, 0)
		var botLeft = Vector3(CameraBounds[0].x, CameraBounds[1].y, 0)
		DebugDraw.draw_line(topLeft, topRight, Color(1, 1, 1), 0.01666)
		DebugDraw.draw_line(topRight, botRight, Color(1, 1, 1), 0.01666)
		DebugDraw.draw_line(botRight, botLeft, Color(1, 1, 1), 0.01666)
		DebugDraw.draw_line(botLeft, topLeft, Color(1, 1, 1), 0.01666)
		
		var topLeftB = Vector3(StageBounds[0].x, StageBounds[0].y, 0)
		var topRightB = Vector3(StageBounds[1].x, StageBounds[0].y, 0)
		var botRightB = Vector3(StageBounds[1].x, StageBounds[1].y, 0)
		var botLeftB = Vector3(StageBounds[0].x, StageBounds[1].y, 0)
		DebugDraw.draw_line(topLeftB, topRightB, Color(1, 0.25, 0.25), 0.01666)
		DebugDraw.draw_line(topRightB, botRightB, Color(1, 0.25, 0.25), 0.01666)
		DebugDraw.draw_line(botRightB, botLeftB, Color(1, 0.25, 0.25), 0.01666)
		DebugDraw.draw_line(botLeftB, topLeftB, Color(1, 0.25, 0.25), 0.01666)
	for point in StageLedgePoints:
		var pointCol = Color(1, 0.1, 0.1)
		if point.dir == StageLedgePoint.LedgeDir.LEFT:
			pointCol = Color(0.1, 0.1, 1)
		DebugDraw.draw_sphere(FHelp.Vec2to3(point.pos), 2, pointCol, 0.016666)

func _network_process(input: Dictionary) -> void:
	var targetPoint := Vector2.ZERO
	var ftMins := Vector2.ZERO
	var ftMaxs := Vector2.ZERO
	var desRatio := 1.777
	var desRatio2 := 0.5625
	for i in range(fighters.size()):
		var wPos = fighters[i].ftPos
		targetPoint = wPos + Vector2(6 * fighters[i].facing, 0)
		if i == 0:
			ftMins = targetPoint
			ftMaxs = targetPoint
		else:
			if targetPoint.x < ftMins.x:
				ftMins.x = targetPoint.x
			if targetPoint.x > ftMaxs.x:
				ftMaxs.x = targetPoint.x
			if targetPoint.y < ftMins.y:
				ftMins.y = targetPoint.y
			if targetPoint.y > ftMaxs.y:
				ftMaxs.y = targetPoint.y
	
	ftMins.x -= 60
	ftMaxs.x += 60
	ftMins.y -= 10
	ftMaxs.y += 70
	
	if ftMins.x < CameraBounds[0].x:
		ftMins.x = CameraBounds[0].x
	if ftMins.y < CameraBounds[0].y:
		ftMins.y = CameraBounds[0].y
	if ftMaxs.x > CameraBounds[1].x:
		ftMaxs.x = CameraBounds[1].x
	if ftMaxs.y > CameraBounds[1].y:
		ftMaxs.y = CameraBounds[1].y
	
	var camCenter = Vector2((ftMins.x + ftMaxs.x) * 0.5, lerp(ftMins.y, ftMaxs.y, 0.35))
	
	var camSX = ftMaxs.x - ftMins.x
	var camSY = ftMaxs.y - ftMins.y
	
	
	if camSX < camSY * desRatio:
		camSX = camSY * desRatio
	if camSY < camSX * desRatio2:
		camSY = camSX * desRatio2
	
	var camMins = Vector2(camCenter.x - camSX * 0.5, camCenter.y - camSY * 0.5)
	var camMaxs = Vector2(camCenter.x + camSX * 0.5, camCenter.y + camSY * 0.5)
	
	if camMins.x < CameraBounds[0].x:
		var diff = camMins.x - CameraBounds[0].x
		camMins.x -= diff
		camMaxs.x -= diff
	if camMaxs.x > CameraBounds[1].x:
		var diff = camMaxs.x - CameraBounds[1].x
		camMins.x -= diff
		camMaxs.x -= diff
	if camMins.y < CameraBounds[0].y:
		var diff = camMins.y - CameraBounds[0].y
		camMins.y -= diff
		camMaxs.y -= diff
	if camMaxs.y > CameraBounds[1].y:
		var diff = camMaxs.y - CameraBounds[1].y
		camMins.y -= diff
		camMaxs.y -= diff
	
	camCenter = Vector2((camMins.x + camMaxs.x) * 0.5, (camMins.y + camMaxs.y) * 0.5)
	
	#DebugDraw.draw_aabb_ab(FHelp.Vec2to3(ftMins), FHelp.Vec2to3(ftMaxs), Color(0.25, 0.25, 1), 0.016666)
	#DebugDraw.draw_aabb_ab(FHelp.Vec2to3(camMins), FHelp.Vec2to3(camMaxs), Color(0.25, 1, 0.25), 0.016666)
	
	var targetDist := maxf(absf(camMins.x - camMaxs.x) * 1.05, 150)
	var desiredPos := FHelp.Vec2to3(camCenter) + Vector3(0, 0, targetDist)
	cameraTransform.origin = cameraTransform.origin.lerp(desiredPos, 0.12)

func _save_state() -> Dictionary:
	return {
		_cameraTransform = cameraTransform
	}
	

func _load_state(state: Dictionary) -> void:
	cameraTransform = state["_cameraTransform"]
