@tool

extends Node3D

class_name Fighter

var lastConstruct = 0.0
@onready var Animator: NetworkAnimationPlayer = $AnimationPlayer
@onready var FighterSkeleton: Skeleton3D = $Skeleton3D
var input_controller: BasePlayer
var stage: PFStage
var badgeGrid
var fighterID : int = -1
@export var UpdateHurtboxes = false:
	set(inHurt):
		construct_hurtboxes()
		UpdateHurtboxes = inHurt

## Hurtboxes are what attack and grab hitboxes test against, to see if you should be hit.
## Try to match them closely to the shape of your character.
@export var Hurtboxes: Array[HurtboxDefinition] = []

## Stats for this fighter's shield.
## Do not modify this through code! It won't affect gameplay!
## If you want to affect your shield at runtime, use 'ourShield' instead.
@export var shield: ShieldDefinition

## These are important properties, used to determine the characteristics of your fighter.
## You can control things such as movement speed and acceleration, weight, falling speed, and more.
## Do not modify this through code! It won't affect gameplay!
## If you want to affect FightTable at runtime, use 'ourFightTable' instead.
@export var FightTable: FighterPropertyTable

## Names of the bones that should be used to calculate the ECB,
## a special structure used to create the character's collision.
## Recommended to select 3-6 bones that will typically represent
## the bounds of your character, like hands/feet/head/tail.
@export var ECB_Bones: Array[String] = []

## Function to run any time the fighter enters a new state.
## Args:
## inFighter: The fighter whose state is changing.
@export var OnStateEnterFuncs: Array[FighterFunction] = []

## Function to run on every frame.
## Args:
## inFighter: The fighter this function is being called on.
@export var OnFrameFuncs: Array[OnFrame] = []

## Function to run when this fighter takes damage.[br]
## Args:[br]
## inFighter: The fighter being damaged.[br]
## inHurtbox: The hurtbox that was struck, belonging to the victim.[br]
## inHurtbox: The hitbox that struck the hurtbox, belonging to the attacker.[br]
## attacker: The fighter which hit the victim.
@export var OnDamageFuncs: Array[OnDamage] = []

## All possible states for the fighter to exist in.
@export var States: Array[FighterState] = []

var ledgeBox = Rect2(Vector2(-16, 6), Vector2(32, 12))
var currentLedge: StageLedgePoint
var currentGround: StageCollisionSegment
var ourShield: ShieldDefinition
var ourFightTable: FighterPropertyTable
var ECB: Array = [[Vector2.ZERO, Vector2.ZERO, Vector2.ZERO, Vector2.ZERO], Vector2.ZERO]
var ECBOld: Array = [[Vector2.ZERO, Vector2.ZERO, Vector2.ZERO, Vector2.ZERO], Vector2.ZERO]
var ActiveHitboxes: Array[HitboxDefinition] = []
var curSubaction: Array[Array]
var stateFlags: int = 0
var FightersHit: Array[Fighter] = []
var GrabbedFighter : int = -1
var facing : int = 1
var kbVel : Vector2 = Vector2.ZERO
var ftVel : Vector2 = Vector2.ZERO
var animVel : Vector2 = Vector2.ZERO
var ftPos : Vector2 = Vector2.ZERO
var grounded : bool = false
var hitLag : int = 0
var hitStun : int = 0
var percentage : float = 0.0
var charState : FighterState
var oldPose : Array[Transform3D] = []
var blendTime : int = 0
var lastStateChange : int = 0
var TransNPhysics : bool = false
var TransNOldPos: Vector3 = Vector3.ZERO
var InterruptableTime : int = 0
var jumps : int = 0
var JustChangedState : bool = false
var internalFrameCounter : int = 0
var lastHitHurtbox : HurtboxDefinition
var lastHitHitbox : HitboxDefinition
var downDesire : String = "U"
var effectiveVel : Vector2 = Vector2.ZERO
var shieldStun : int = 0
var lastTimeOnLedge : int = 0
var desiredAnim : String = ""



func _ready():
	if Engine.is_editor_hint():
		construct_hurtboxes()

func check_fighter_flag(num: int) -> bool:
	var flag = 1 << num
	var bitAnd = stateFlags & flag
	return bitAnd > 0

func set_fighter_flag(num: int, inBool: bool) -> void:
	if check_fighter_flag(num) != inBool:
		var flag = 1 << num
		stateFlags = stateFlags ^ flag

func update_ui_percent() -> void:
	badgeGrid.update_player_percent(self)

func _network_spawn(data: Dictionary) -> void:
	position = data["position"]
	input_controller = data["controller"]
	ourShield = shield.duplicate(true)
	ourFightTable = FightTable.duplicate(true)
	FighterSkeleton = $Skeleton3D
	set_body_vulnerability(HurtboxDefinition.hurtboxBodyState.Normal)
	grounded = true
	_change_fighter_state(States[0])

func respawn_self() -> void:
	percentage = 0
	ftPos = Vector2.ZERO
	kbVel = Vector2.ZERO
	grounded = true
	set_body_vulnerability(HurtboxDefinition.hurtboxBodyState.Normal)
	_change_fighter_state(find_state_by_name("Wait"), 0, 0)
	_calculate_ecb(true)
	_calculate_ecb(true)
	update_ui_percent()

func set_body_vulnerability(inState: HurtboxDefinition.hurtboxBodyState) -> void:
	for hb in Hurtboxes:
		hb.bodyState = inState


func _process(delta: float) -> void:
	if Engine.is_editor_hint():
		if Time.get_unix_time_from_system() > lastConstruct + 0.01666 and UpdateHurtboxes:
			construct_hurtboxes()
			lastConstruct = Time.get_unix_time_from_system()

func clear_hitboxes() -> void:
	ActiveHitboxes.clear()
	FightersHit.clear()

func get_frame_in_state() -> int:
	return internalFrameCounter - lastStateChange

func get_shield_size() -> float:
	var shieldPos = FighterSkeleton.get_bone_global_pose(FighterSkeleton.find_bone("Extra"))
	var shieldHealthRatio = ourShield.health / ourShield.maxHealth
	var shieldSizeHard = lerp(ourShield.endSizeHardShield, ourShield.startSizeHardShield, shieldHealthRatio)
	var shieldSizeLight = lerp(ourShield.endSizeLightShield, ourShield.startSizeLightShield, shieldHealthRatio)
	var triggerWithDeadzone = move_toward(input_controller.get_trigger_analog_unbuffered(), 0, 0.1) * 1.1111
	return lerp(shieldSizeLight, shieldSizeHard, triggerWithDeadzone)

func get_shield_position() -> Vector3:
	var shieldPos = FighterSkeleton.get_bone_global_pose(FighterSkeleton.find_bone("Extra"))
	return FighterSkeleton.to_global(shieldPos.origin)

func draw_ecb() -> void:
	var ECBOldBot = Vector3(0,0,0)
	var ECBOldLeft = Vector3(ECBOld[0][0].x, ECBOld[0][0].y,0)
	var ECBOldTop = Vector3(0, ECBOld[0][1].y,0)
	var ECBOldRight = Vector3(ECBOld[0][2].x, ECBOld[0][2].y,0)
	var oldPos = Vector3(ECBOld[1].x, ECBOld[1].y, 2)
	DebugDraw.draw_line(oldPos, oldPos + ECBOldLeft, Color(1, 0.25, 0, 1), 0.016666)
	DebugDraw.draw_line(oldPos + ECBOldLeft, oldPos + ECBOldTop, Color(1, 0.25, 0, 1), 0.016666)
	DebugDraw.draw_line(oldPos + ECBOldTop, oldPos + ECBOldRight, Color(1, 0.25, 0, 1), 0.016666)
	DebugDraw.draw_line(oldPos + ECBOldRight, oldPos + ECBOldBot, Color(1, 0.25, 0, 1), 0.016666)
	
	var ECBBot = Vector3(0,0,0)
	var ECBLeft = Vector3(ECB[0][0].x, ECB[0][0].y,0)
	var ECBTop = Vector3(0, ECB[0][1].y,0)
	var ECBRight = Vector3(ECB[0][2].x, ECB[0][2].y,0)
	var newPos = FHelp.Vec2to3(ftPos)
	DebugDraw.draw_line(newPos, newPos + ECBLeft, Color(1, 1, 0, 1), 0.016666)
	DebugDraw.draw_line(newPos + ECBLeft, newPos + ECBTop, Color(1, 1, 0, 1), 0.016666)
	DebugDraw.draw_line(newPos + ECBTop, newPos + ECBRight, Color(1, 1, 0, 1), 0.016666)
	DebugDraw.draw_line(newPos + ECBRight, newPos + ECBBot, Color(1, 1, 0, 1), 0.016666)

func _calculate_ecb(updateOld: bool) -> void:
	var highestPos = 0.0
	var leftmostPos = 0.0
	var rightmostPos = 0.0
	for i in range(ECB_Bones.size()):
		var boneName = ECB_Bones[i]
		var bonePosGlobal = FighterSkeleton.to_global(FighterSkeleton.get_bone_global_pose(FighterSkeleton.find_bone(boneName)).origin)
		bonePosGlobal -= position
		if bonePosGlobal.x <= leftmostPos:
			leftmostPos = bonePosGlobal.x
		if bonePosGlobal.x >= rightmostPos:
			rightmostPos = bonePosGlobal.x
		if bonePosGlobal.y > highestPos:
			highestPos = bonePosGlobal.y
	var ECBLeft = Vector2(leftmostPos, highestPos * 0.5)
	var ECBTop = Vector2(0, highestPos)
	var ECBRight = Vector2(rightmostPos, highestPos * 0.5)
	ECB[0][0] = ECBLeft
	ECB[0][1] = ECBTop
	ECB[0][2] = ECBRight
	ECB[1] = ftPos
	if updateOld:
		ECBOld = ECB.duplicate(true)

enum SurfaceSlope{
	WALL,
	FLOOR,
	CEIL
}

func get_surface_type(inNormal: Vector2) -> int:
	if inNormal.y > 0.1:
		return SurfaceSlope.FLOOR
	else: if inNormal.y > -0.85:
		return SurfaceSlope.WALL
	return SurfaceSlope.CEIL

func apply_traction() -> void:
	ftVel.x -= ftVel.x * FightTable.Friction
	ftVel.x = move_toward(ftVel.x, 0, FightTable.Friction * 0.05)

func apply_drag() -> void:
	ftVel.x -= ftVel.x * FightTable.AerialFriction
	ftVel.x = move_toward(ftVel.x, 0, FightTable.AerialFriction * 0.05)

func do_fighter_move_and_collide(desiredPos: Vector2):
	ftPos = desiredPos
	var oldGround = currentGround
	var wasGrounded = grounded
	_calculate_ecb(false)
	depen_fighter_from_corners(0)
	try_fighter_ccd()
	depen_fighter_by_midpoint()
	if wasGrounded:
		var onGround = check_and_move_to_ground()
		if !onGround:
			#print("no ground!")
			if !charState.allowSlideoff:
				#print("preventing slideoff")
				var closest = oldGround.startOffset
				if (oldGround.endOffset - ftPos).length() < (oldGround.startOffset - ftPos).length():
					closest = oldGround.endOffset
				#DebugDraw.draw_sphere(FHelp.Vec2to3(ftPos), 1, Color(1, 0, 0), 0.016666)
				ftPos = closest
				#DebugDraw.draw_sphere(FHelp.Vec2to3(ftPos), 1, Color(0, 1, 0), 0.016666)
				animVel = Vector2.ZERO
				ftVel = Vector2.ZERO
				grounded = true
			else:
				#print("go airborne")
				check_on_airborne()
	#DebugDraw.draw_sphere(FHelp.Vec2to3(ftPos), 0.5, Color(1, 1, 1), 0.016666)
	#draw_ecb()
	_calculate_ecb(true)

func depen_fighter_by_midpoint() -> void:
	for i in range(ECB[0].size()):
		for s in range(stage.StageCollisionSegments.size()):
			var segment = stage.StageCollisionSegments[s]
			if segment.colType == StageCollisionSegment.collisionType.semisolid:
				continue
			var segDir = (segment.endOffset - segment.startOffset).rotated(PI * 0.5).normalized()
			if (i == 0 or i == 2) and get_surface_type(segDir) == SurfaceSlope.FLOOR:
				continue
			if i == 1 and get_surface_type(segDir) != SurfaceSlope.CEIL:
				continue
			if i == 3 and get_surface_type(segDir) != SurfaceSlope.FLOOR:
				continue
			var mid = ftPos + ECB[0][1] * 0.5
			var dirFromMid = (ECB[0][i] - ECB[0][1] * 0.5).normalized()
			if dirFromMid.dot(segDir) > 0:
				continue
			var traceLength = (ECB[0][i] - ECB[0][1] * 0.5).length()
			#DebugDraw.draw_line(FHelp.Vec2to3(mid), FHelp.Vec2to3(mid + dirFromMid * traceLength), Color(1, 1, 1), 0.016666)
			var colResult = FHelp.TestRayLineIntersection(mid, dirFromMid, segment.startOffset, segment.endOffset)
			if colResult.hit and colResult.dist <= traceLength:
				var hitPoint = mid + dirFromMid * colResult.dist
				#DebugDraw.draw_sphere(FHelp.Vec2to3(hitPoint), 0.2, Color(1, 0, 0), 0.01666)
				var move = -dirFromMid * ((traceLength - colResult.dist) + 0.1)
				ftPos += move
				_calculate_ecb(false)
				if i == 3 and effectiveVel.y < -1:
					currentGround = segment
					check_on_landing()

func depen_fighter_from_corners(iter: int) -> void:
	if iter > 4: return
	var halfSpaces = [[ECB[0][0], ECB[0][1]], [ECB[0][1], ECB[0][2]], [ECB[0][2], ECB[0][3]], [ECB[0][3], ECB[0][0]]]
	halfSpaces = halfSpaces.duplicate(true)
	var depenned = false
	for i in range(halfSpaces.size()):
		halfSpaces[i].append((halfSpaces[i][1] - halfSpaces[i][0]).normalized().rotated(PI * -0.5))
		halfSpaces[i][0] += ftPos
		halfSpaces[i][1] += ftPos
	for s in range(stage.StageCollisionSegments.size()):
		if depenned:
			break
		var seg = stage.StageCollisionSegments[s]
		if seg.colType == StageCollisionSegment.collisionType.semisolid:
			continue
		var tests = [seg.startOffset, seg.endOffset]
		for test in range(tests.size()):
			var t = tests[test]
			if depenned:
				break
			var inside = true
			var closestDist = 1000
			var depenDir = Vector2.UP
			for hs in range(halfSpaces.size()):
				var i = halfSpaces[hs]
				if depenned:
					break
				#DebugDraw.draw_line(FHelp.Vec2to3(i[0]), FHelp.Vec2to3(i[1]), Color(0, 1, 1), 0.01666)
				#DebugDraw.draw_arrow_line(FHelp.Vec2to3((i[0] + i[1]) * 0.5), FHelp.Vec2to3((i[0] + i[1]) * 0.5 + i[2] * 2.5), Color(0, 1, 1), 0.4, 0.01666)
				var offset = t - i[0]
				var dist = offset.dot(i[2])
				if dist < 0:
					inside = false
				else:
					if dist < closestDist:
						closestDist = dist
						depenDir = i[2]
			if inside:
				var side = sign((t - ECB[1]).x)
				if side == 1:
					var test1 = halfSpaces[1]
					var test2 = halfSpaces[2]
					var intersect1 = FHelp.LineIntersection(t, t + Vector2.RIGHT, test1[0], test1[1]) - t
					var intersect2 = FHelp.LineIntersection(t, t + Vector2.RIGHT, test2[0], test2[1]) - t
					ftPos += Vector2.LEFT * min(intersect1.length(), intersect2.length())
					depenned = true
				else:
					var test1 = halfSpaces[0]
					var test2 = halfSpaces[3]
					var intersect1 = FHelp.LineIntersection(t, t + Vector2.LEFT, test1[0], test1[1]) - t
					var intersect2 = FHelp.LineIntersection(t, t + Vector2.LEFT, test2[0], test2[1]) - t
					ftPos += Vector2.RIGHT * min(intersect1.length() + 0.1, intersect2.length() + 0.1)
					depenned = true
				#DebugDraw.draw_sphere(FHelp.Vec2to3(t), 0.2, Color(1, 0, 0), 0.01666)
	if depenned:
		_calculate_ecb(false)
		depen_fighter_from_corners(iter + 1)

func try_fighter_ccd() -> void:
	for p in range(ECB[0].size()):
		# 0 = left
		# 1 = top
		# 2 = right
		# 3 = bot
		var oldP = ECBOld[0][p] + ECBOld[1]
		var desP = ECB[0][p] + ECB[1]
		var dir = (desP - oldP).normalized()
		var traceLength = oldP.distance_to(desP)
		for s in range(stage.StageCollisionSegments.size()):
			var segment = stage.StageCollisionSegments[s]
			var segDir = (segment.endOffset - segment.startOffset).rotated(PI * 0.5).normalized()
			if get_frame_in_state() >= InterruptableTime and segment.colType == StageCollisionSegment.collisionType.semisolid and input_controller.get_movement_vector_unbuffered().y > 0.85:
				continue
			if dir.dot(segDir) > 0:
				continue
			if get_surface_type(segDir) == SurfaceSlope.WALL and (p != 0 and p != 2):
				continue
			if get_surface_type(segDir) == SurfaceSlope.FLOOR and (p == 1):
				continue
			if get_surface_type(segDir) == SurfaceSlope.CEIL and (p == 3):
				continue
			
			var colResult = FHelp.TestRayLineIntersection(oldP, dir, segment.startOffset, segment.endOffset)
			if colResult.hit and colResult.dist <= traceLength:
				var depenDir = Vector2.LEFT
				match p:
					1:
						depenDir = Vector2.DOWN
					2:
						depenDir = Vector2.RIGHT
					3:
						depenDir = Vector2.UP
				var depenPoint = FHelp.LineIntersection(desP, desP + depenDir, segment.startOffset, segment.endOffset)
				# todo: just use a regular ray line intersection test, and if it misses,
				# assume we slid past, and move the player to either t2=0 or t2=1 by the point we're testing
				if is_nan(depenPoint.x) or is_nan(depenPoint.y) or is_inf(depenPoint.x) or is_inf(depenPoint.y):
					print("line intersect was NAN")
					continue
				ftPos += -depenDir * ((depenPoint - desP).length() + 0.1)
				if p == 3:
					if !grounded and get_frame_in_state() > 2 and kbVel.length_squared() > 0:
						print("Projecting KBvel!")
						kbVel = kbVel + segDir * -kbVel.dot(segDir)
					check_on_landing()
					currentGround = segment
				_calculate_ecb(false)

func construct_hurtboxes():
	if !Engine.is_editor_hint():
		return
	clear_hitboxes()
	for child in FighterSkeleton.get_children():
		if child is Hurtbox:
			child.queue_free()
	for hurtboxDef in Hurtboxes: 
		if !hurtboxDef:
			continue
		if FighterSkeleton.find_bone(hurtboxDef.boneName) == -1:
			continue
		var newHurtbox = preload("res://scenes/hurtbox.tscn")
		var newHurtboxInstance = newHurtbox.instantiate()
		newHurtboxInstance.boneName = hurtboxDef.boneName
		newHurtboxInstance.startOffset = hurtboxDef.startOffset
		newHurtboxInstance.endOffset = hurtboxDef.endOffset
		newHurtboxInstance.radius = hurtboxDef.radius
		FighterSkeleton.add_child(newHurtboxInstance)
		newHurtboxInstance.position_debug_helpers()
	var anim = $AnimationPlayer as NetworkAnimationPlayer
	var oldAnimPos = anim.current_animation_position
	var frame = int(anim.current_animation_position * 60)
	unfold_action(find_state_by_anim(anim.assigned_animation).action)
	#print(curSubaction)
	for i in range(frame + 1):
		anim.seek(float(i) / 60.0, true)
		update_pose()
		if i < curSubaction.size():
			for subaction in curSubaction[i]:
				if subaction:
					subaction._execute(self)
		for hbd in range(ActiveHitboxes.size()):
			var hitboxDef = ActiveHitboxes[hbd]
			if !ActiveHitboxes:
				continue
			if FighterSkeleton.find_bone(hitboxDef.boneName) == -1:
				continue
			var boneID = FighterSkeleton.find_bone(hitboxDef.boneName)
			if boneID == -1:
				break
			var bonePose = FighterSkeleton.get_bone_global_pose(boneID)
			var globalPos = hitboxDef.offset
			globalPos = bonePose * globalPos
			globalPos = FighterSkeleton.to_global(globalPos)
			if hitboxDef.firstFrame:
				hitboxDef.firstFrame = false
				hitboxDef.curGlobalPosition = globalPos
				hitboxDef.oldGlobalPosition = globalPos
			else:
				hitboxDef.oldGlobalPosition = hitboxDef.curGlobalPosition
				hitboxDef.curGlobalPosition = globalPos
	anim.seek(oldAnimPos, true)
	update_pose()
	#print(ActiveHitboxes)
	for hbd in range(ActiveHitboxes.size()):
		var hitboxDef = ActiveHitboxes[hbd]
		if !ActiveHitboxes:
			continue
		if FighterSkeleton.find_bone(hitboxDef.boneName) == -1:
			continue
		var boneID = FighterSkeleton.find_bone(hitboxDef.boneName)
		if boneID == -1:
			break
		var newHitbox = preload("res://scenes/hurtbox.tscn")
		var newHitboxInstance = newHitbox.instantiate()
		newHitboxInstance.boneName = hitboxDef.boneName
		newHitboxInstance.startOffset = hitboxDef.oldGlobalPosition
		newHitboxInstance.endOffset = hitboxDef.curGlobalPosition
		newHitboxInstance.radius = hitboxDef.radius
		newHitboxInstance.isHitbox = true
		newHitboxInstance.angle = hitboxDef.kbAngle
		FighterSkeleton.add_child(newHitboxInstance)
		newHitboxInstance.position_debug_helpers()
		var desiredColor = Color(0.5, 0.0, 0.0)
		if hitboxDef.IsTrigger:
			desiredColor = Color(0.35, 0.0, 0.35)
		for child in newHitboxInstance.get_children():
			var kfg = child as MeshInstance3D
			kfg.set_instance_shader_parameter("bubbleColor", desiredColor)

func unfold_action(inAction: Array[Subaction]) -> void:
	curSubaction.clear()
	var subactionLength := 0
	var Unfolded : Array[Array] = []
	for i in range(inAction.size()):
		if inAction[i] is SetLoop:
			
			var maxLoopSize = 100
			var curLoopLength = 1
			var loopActive = true
			
			while loopActive:
				if inAction[i + curLoopLength] is SynchronousTimer:
					subactionLength += inAction[i + curLoopLength].frames * (inAction[i].LoopCount - 1)
				if inAction[i + curLoopLength] is ExecuteLoop:
					loopActive = false
					i = i + curLoopLength
				curLoopLength += 1
				if curLoopLength >= maxLoopSize:
					loopActive = false
					i += 1 # skip it
					print("Infinite loop?")
			
		if inAction[i] is SynchronousTimer:
			subactionLength += inAction[i].frames
			continue
			
		if inAction[i] is AsynchronousTimer:
			if subactionLength < inAction[i].frames:
				subactionLength = inAction[i].frames
			continue
			
	
	for i in range(subactionLength + 1):
		Unfolded.append([])
	
	var curIndex := 0
	var loopCount := 0
	var loopStart := 0
	var index = 0
	#print("Starting action parse")
	while index < inAction.size():
		#print(str(index) + ": " + inAction[index].SubactionName)
		if inAction[index] is SetLoop:
			loopStart = index + 1
			loopCount = inAction[index].LoopCount - 1
			index += 1
			continue
		if inAction[index] is ExecuteLoop:
			if loopCount > 0:
				index = loopStart
				loopCount -= 1
			else:
				index += 1
			#print("Loop activated. Going back to " + str(i))
			continue
		if inAction[index] is SynchronousTimer:
			curIndex += inAction[index].frames
			index += 1
			continue
		if inAction[index] is AsynchronousTimer:
			if curIndex < inAction[index].frames:
				curIndex = inAction[index].frames
			index += 1
			continue
		Unfolded[curIndex].append(inAction[index])
		index += 1
	#print("Action parsed!")
	#print(Unfolded.size())
	curSubaction = Unfolded

func set_animation(inAnim: String):
	Animator.current_animation = inAnim
	Animator.assigned_animation = inAnim
	Animator.seek(0, true)
	update_pose()
	

func blend_pose(anim1: String, anim2: String, frame1: int, frame2: int, blend: float) -> void:
	desiredAnim = anim1
	var pose1 = []
	var pose2 = []
	pose1.resize(FighterSkeleton.get_bone_count())
	pose2.resize(FighterSkeleton.get_bone_count())
	Animator.current_animation = anim1
	Animator.assigned_animation = anim1
	Animator.seek(float(frame1) / 60, true)
	update_pose()
	for i in range(FighterSkeleton.get_bone_count()):
		pose1[i] = FighterSkeleton.get_bone_pose(i)
	Animator.current_animation = anim2
	Animator.assigned_animation = anim2
	Animator.seek(float(frame2) / 60, true)
	update_pose()
	for i in range(FighterSkeleton.get_bone_count()):
		pose2[i] = FighterSkeleton.get_bone_pose(i)
	for i in range(FighterSkeleton.get_bone_count()):
		var blendTransform = pose1[i].interpolate_with(pose2[i], blend)
		FighterSkeleton.set_bone_pose_position(i, blendTransform.origin)
		FighterSkeleton.set_bone_pose_rotation(i, blendTransform.basis.get_rotation_quaternion())
		FighterSkeleton.set_bone_pose_scale(i, blendTransform.basis.get_scale())
	update_pose()

func _change_fighter_state(inState: FighterState, blend: int = 0, lag: int = 0) -> void:
	JustChangedState = true
	oldPose.clear()
	oldPose.resize(FighterSkeleton.get_bone_count())
	for i in range(FighterSkeleton.get_bone_count()):
		oldPose[i] = FighterSkeleton.get_bone_pose(i)
	FighterSkeleton.reset_bone_poses()
	lastStateChange = internalFrameCounter
	charState = inState
	blendTime = blend
	InterruptableTime = lag
	for i in range(OnStateEnterFuncs.size()):
		OnStateEnterFuncs[i]._execute(self)
	for i in range(charState.onEnterState.size()):
		charState.onEnterState[i]._execute(self)
	
	TransNPhysics = charState.useAnimPhysics
	if TransNPhysics:
		var TransN = FighterSkeleton.find_bone("TransN")
		var TransNNewTransform = FighterSkeleton.get_bone_global_pose(TransN)
		var TransNPos = FighterSkeleton.to_global(TransNNewTransform.origin)
		TransNPos -= position
		TransNOldPos = TransNPos
	
	if blendTime > 0:
		#print("Blending at tick " + str(internalFrameCounter))
		var ratio = float(get_frame_in_state() + 1) / blendTime
		for i in range(FighterSkeleton.get_bone_count()):
			var blendTransform = oldPose[i].interpolate_with(FighterSkeleton.get_bone_pose(i), ratio)
			FighterSkeleton.set_bone_pose_position(i, blendTransform.origin)
			FighterSkeleton.set_bone_pose_rotation(i, blendTransform.basis.get_rotation_quaternion())
			FighterSkeleton.set_bone_pose_scale(i, blendTransform.basis.get_scale())
	update_pose()
	execute_current_action(0)

func find_state_by_name(inState: String) -> FighterState:
	for fstate in States:
		if fstate.stateName == inState:
			return fstate
	return null

func find_state_by_anim(inAnim: String) -> FighterState:
	for fstate in States:
		if fstate.stateAnim == inAnim:
			return fstate
	return null

func _network_process(input: Dictionary) -> void:
	pass

func do_damage(inHurtbox: HurtboxDefinition, inHitbox: HitboxDefinition, victim: Fighter) -> void:
	FightersHit.append(victim)
	for i in range(victim.OnDamageFuncs.size()):
		victim.OnDamageFuncs[i]._execute(victim, inHurtbox, inHitbox, self)

func check_hitboxes() -> void:
	for i in range(ActiveHitboxes.size()):
		if i >= ActiveHitboxes.size():
			break
		var curHitbox = ActiveHitboxes[i] as HitboxDefinition
		var boneID = FighterSkeleton.find_bone(curHitbox.boneName)
		if boneID == -1:
			break
		var bonePose = FighterSkeleton.get_bone_global_pose(boneID)
		var globalPos = curHitbox.offset
		globalPos = bonePose * globalPos
		globalPos = FighterSkeleton.to_global(globalPos)
		if curHitbox.firstFrame:
			curHitbox.firstFrame = false
			curHitbox.curGlobalPosition = globalPos
			curHitbox.oldGlobalPosition = globalPos
		else:
			curHitbox.oldGlobalPosition = curHitbox.curGlobalPosition
			curHitbox.curGlobalPosition = globalPos
		
		if curHitbox.oldGlobalPosition == curHitbox.curGlobalPosition:
			curHitbox.oldGlobalPosition += Vector3(0, 0.001, 0)
		
		#DebugDraw.draw_sphere(curHitbox.curGlobalPosition, curHitbox.radius, Color(1, 0, 0), 0.01666)
		for f in range(stage.fighters.size()):
			if curHitbox == null or !curHitbox:
				break
			var curFighter = stage.fighters[f] as Fighter
			if curFighter == self:
				continue
			var alreadyHit = false
			for t in range(FightersHit.size()):
				if FightersHit[t] == curFighter:
					alreadyHit = true
			if alreadyHit:
				continue
			if !curHitbox.IsTrigger and curFighter.ourShield.active:
				var ss = curFighter.get_shield_size()
				var sp = curFighter.get_shield_position()
				var hitShield = FHelp.TestSphereCapsuleIntersection(sp, ss, curHitbox.curGlobalPosition, curHitbox.oldGlobalPosition, curHitbox.radius)
				if hitShield:
					FightersHit.append(curFighter)
					apply_shielded_hit_effects(curFighter, curHitbox)
					continue
			for h in range(curFighter.Hurtboxes.size()):
				var curHurtbox = curFighter.Hurtboxes[h] as HurtboxDefinition
				if curHurtbox.bodyState == HurtboxDefinition.hurtboxBodyState.Intangible:
					print("INTANGIBLE")
					return
				var hurtboxBonePose = curFighter.FighterSkeleton.get_bone_global_pose(FighterSkeleton.find_bone(curHurtbox.boneName))
				var globalStart = curHurtbox.startOffset
				var globalEnd = curHurtbox.endOffset
				globalStart = hurtboxBonePose * globalStart
				globalEnd = hurtboxBonePose * globalEnd
				globalStart = curFighter.FighterSkeleton.to_global(globalStart)
				globalEnd = curFighter.FighterSkeleton.to_global(globalEnd)
				var intersect = FHelp.TestCapsuleCapsuleIntersection(curHitbox.curGlobalPosition, curHitbox.oldGlobalPosition, curHitbox.radius, globalStart, globalEnd, curHurtbox.radius)
				if intersect:
					if curHitbox.IsTrigger:
						for tf in range(curHitbox.TriggerFuncs.size()):
							curHitbox.TriggerFuncs[tf]._execute(curHurtbox, curHitbox, curFighter, self)
					else:
						do_damage(curHurtbox, curHitbox, curFighter)
					break

func check_interrupts(recursion: int) -> void:
	if recursion > 3:
		print("Stopped an interrupt loop.")
		return
	if hitStun > 0:
		return
	var interruptedPrematurely = false
	for i in range(charState.interrupts.size()):
		if get_frame_in_state() >= InterruptableTime or charState.interrupts[i].alwaysActive:
			if charState.interrupts[i].startActive:
				if charState.interrupts[i].disableTime != 0 and get_frame_in_state() >= charState.interrupts[i].disableTime:
					if charState.interrupts[i].enableTime == 0:
						continue
					else:
						if get_frame_in_state() < charState.interrupts[i].enableTime:
							continue
			else:
				if get_frame_in_state() < charState.interrupts[i].enableTime:
					continue
				if charState.interrupts[i].disableTime != 0 and get_frame_in_state() >= charState.interrupts[i].disableTime:
					continue
			if grounded:
				if charState.interrupts[i].requiredState == Interrupt.groundRequire.OnlyAirborne:
					continue
			else:
				if charState.interrupts[i].requiredState == Interrupt.groundRequire.OnlyGrounded:
					continue
			var interrupted = charState.interrupts[i]._execute(self)
			if interrupted: 
				#print("Interrupted into " + charState.stateName)
				interruptedPrematurely = true
				break
	if interruptedPrematurely:
		check_interrupts(recursion + 1)

func check_onframe() -> void:
	for i in range(OnFrameFuncs.size()):
		if JustChangedState:
			JustChangedState = false
			check_onframe()
			break
		OnFrameFuncs[i]._execute(self)
	for i in range(charState.onFrame.size()):
		if JustChangedState:
			JustChangedState = false
			check_onframe()
			break
		charState.onFrame[i]._execute(self)

func execute_current_action(actionFrame: int) -> void:
	if curSubaction.size() > actionFrame:
		#DebugDraw.set_text("ACTION TIMER", str(actionFrame))
		for i in range(curSubaction[actionFrame].size()):
			var subaction = curSubaction[actionFrame][i]
			#DebugDraw.set_text("SUBACTION", subaction.SubactionName)
			subaction._execute(self)

func update_pose() -> void:
	FighterSkeleton.force_update_bone_child_transform(0)
	#for i in range(FighterSkeleton.get_bone_count()):
	#	FighterSkeleton.force_update_bone_child_transform(0)

func apply_shielded_hit_effects(inVictim: Fighter, inHitbox: HitboxDefinition) -> void:
	var d = inHitbox.damage + inHitbox.damageShield
	var s = max(input_controller.get_trigger_analog_unbuffered(), 0.3)
	var a = 0.65 * (1 - ((s - 0.3) / 0.7))
	inVictim.shieldStun = 200 / 201 * (((d * (a + 0.3)) * 1.5) + 2)
	var side = sign((inVictim.ftPos - ftPos).x)
	inVictim.ftVel += Vector2((d * 0.09 + 0.4) * side, 0)
	ftVel += Vector2((d * 0.04 + 0.025) * -side, 0)
	var c = 1
	var e = 1
	var ourHitlag = floor(c * floor(e * floor(3+inHitbox.damage/3)))
	hitLag = ourHitlag
	inVictim.hitLag = ourHitlag
	inVictim.ourShield.health = max(inVictim.ourShield.health - d, 0)

func check_on_airborne() -> void:
	if grounded:
		grounded = false
		for i in range(charState.onAirborne.size()):
			charState.onAirborne[i]._execute(self)

func check_on_landing() -> void:
	if !grounded:
		grounded = true
		for i in range(charState.onLanding.size()):
			charState.onLanding[i]._execute(self)

func check_and_move_to_ground() -> bool:
	var hitAny = false
	for s in range(stage.StageCollisionSegments.size()):
		var segment = stage.StageCollisionSegments[s]
		var segDir = (segment.endOffset - segment.startOffset).rotated(PI * 0.5).normalized()
		if get_surface_type(segDir) == SurfaceSlope.FLOOR:
			var testDist = 3
			var testStartPos = ftPos + Vector2.DOWN
			var traceResult = FHelp.TestRayLineIntersection(testStartPos, Vector2.UP, segment.startOffset, segment.endOffset)
			#DebugDraw.draw_line(FHelp.Vec2to3(testStartPos), FHelp.Vec2to3(testStartPos + Vector2.UP * testDist), Color(0, 0, 255), 0.01666)
			if traceResult.hit and traceResult.dist <= testDist:
				kbVel.y = 0
				hitAny = true
				currentGround = segment
				#print("we hit yay")
				var hitPos = testStartPos + Vector2.UP * traceResult.dist
				ftPos = hitPos + Vector2.DOWN * 0.1
	return hitAny

func fighter_tick() -> void:
	#DebugDraw.set_text("ACTION TIMER", "NO ACTION")
	#DebugDraw.set_text("SUBACTION", "NO SUBACTION")
	if input_controller:
		if hitLag > 0 or shieldStun > 0:
			if hitLag > 0:
				hitLag -= 1
			if hitLag == 0:
				internalFrameCounter += 1
				#DebugDraw.draw_arrow_line(FHelp.Vec2to3(ftPos), FHelp.Vec2to3(ftPos + kbVel.normalized() * 32), Color(0.3, 0.3, 0.3), 0.1, false, 3)
				var kbVelPerpendicular = kbVel.rotated(PI * 0.5).normalized()
				var moveRestrict = input_controller.get_movement_vector_unbuffered()
				moveRestrict.y *= -1
				if moveRestrict.length() > 1:
					moveRestrict = moveRestrict.normalized()
				var DirectionalInfluencePower = kbVelPerpendicular.dot(moveRestrict)
				var DIColor = Color(1, lerp(1, 0, abs(DirectionalInfluencePower)), lerp(1, 0, abs(DirectionalInfluencePower)))
				var maxDIAng = deg_to_rad(18)
				kbVel = kbVel.rotated(maxDIAng * DirectionalInfluencePower)
				#DebugDraw.draw_arrow_line(FHelp.Vec2to3(ftPos), FHelp.Vec2to3(ftPos + moveRestrict * 32), Color(0, 1, 0), 0.1, false, 3)
				#DebugDraw.draw_arrow_line(FHelp.Vec2to3(ftPos), FHelp.Vec2to3(ftPos + kbVel.normalized() * 32), DIColor, 0.1, false, 3)
			if shieldStun > 0:
				shieldStun -= 1
		else:
			internalFrameCounter += 1
			if hitStun > 0:
				hitStun -= 1
		
		if !ourShield.active:
			ourShield.health = min(ourShield.health + 0.1, ourShield.maxHealth)
		
		Animator.current_animation = desiredAnim
		Animator.assigned_animation = desiredAnim
		Animator.seek(float(get_frame_in_state() / 60.0), true)
		
		if get_frame_in_state() > Animator.current_animation_length * 60 and charState.onAnimFinishedState != "":
			_change_fighter_state(find_state_by_name(charState.onAnimFinishedState))
		
		#DebugDraw.set_text("Input Velocity: ", str(input_controller.get_movement_vector_velocity_unbuffered()))
		
		if get_frame_in_state() < blendTime:
			for i in range(FighterSkeleton.get_bone_count()):
				var ratio = float(get_frame_in_state() + 1) / blendTime
				var blendTransform = oldPose[i].interpolate_with(FighterSkeleton.get_bone_pose(i), ratio)
				FighterSkeleton.set_bone_pose_position(i, blendTransform.origin)
				FighterSkeleton.set_bone_pose_rotation(i, blendTransform.basis.get_rotation_quaternion())
				FighterSkeleton.set_bone_pose_scale(i, blendTransform.basis.get_scale())
		
		var TransNMotion = Transform3D.IDENTITY
		var TransN = FighterSkeleton.find_bone("TransN")
		
		if TransNPhysics:
			var TransNNewTransform = FighterSkeleton.get_bone_global_pose(TransN)
			var TransNPos = FighterSkeleton.to_global(TransNNewTransform.origin)
			TransNPos -= FHelp.Vec2to3(ftPos)
			TransNMotion = TransNPos - TransNOldPos
			TransNOldPos = TransNPos
			#print(TransNMotion)
			animVel = FHelp.Vec3to2(TransNMotion)
		else:
			ftVel += animVel
			animVel = Vector2.ZERO
		
		if hitLag == 0 and shieldStun == 0:
			check_interrupts(0)
			
			var wasGrounded = grounded
			var oldGround = currentGround
			
			check_onframe()
			
			FighterSkeleton.set_bone_pose_position(TransN, Vector3.ZERO)
			FighterSkeleton.set_bone_pose_rotation(TransN, Quaternion.IDENTITY)
			FighterSkeleton.set_bone_pose_scale(TransN, Vector3.ONE)
			
			update_pose()
			
			effectiveVel = ftVel + kbVel + animVel
			
			do_fighter_move_and_collide(ftPos + effectiveVel)
			
			if get_frame_in_state() != 0:
				execute_current_action(get_frame_in_state())
			
			kbVel = kbVel.move_toward(Vector2.ZERO, 0.051)
			
			check_hitboxes()
		else:
			FighterSkeleton.set_bone_pose_position(TransN, Vector3.ZERO)
			FighterSkeleton.set_bone_pose_rotation(TransN, Quaternion.IDENTITY)
			FighterSkeleton.set_bone_pose_scale(TransN, Vector3.ONE)
			update_pose()
		
	if ftPos.x < stage.StageBounds[0].x or ftPos.x > stage.StageBounds[1].x or ftPos.y < stage.StageBounds[0].y or ftPos.y > stage.StageBounds[1].y:
		respawn_self()
	position = FHelp.Vec2to3(ftPos)
	#var ss = get_shield_size()
	#var sp = get_shield_position()
	#DebugDraw.draw_sphere(sp, ss, Color(0.2, 0.5, 1), 0.01666)
	if hitStun > 0 and hitLag > 0:
		var RNG = RandomNumberGenerator.new()
		position.x += RNG.randf_range(-2, 2)
		var SDIPulse = input_controller.get_movement_vector_velocity_unbuffered()
		var SDIPulseAllow = SDIPulse.dot(input_controller.get_movement_vector_unbuffered()) >= 0
		SDIPulse.y *= -1
		SDIPulse *= 1.5
		if SDIPulseAllow:
			do_fighter_move_and_collide(ftPos + SDIPulse)
		check_on_airborne()
	#DebugDraw.set_text("BUTTONS", input_controller.shield_down())
	#DebugDraw.set_text("STATE", charState.stateName)
	#DebugDraw.set_text("FLAGS: ", str(stateFlags))

func _save_state() -> Dictionary:
	var state = {
		_ourShield = ourShield.duplicate(true),
		_ourFightTable = ourFightTable.duplicate(true),
		_ECB = ECB.duplicate(true),
		_ECBOld = ECBOld.duplicate(true),
		_ActiveHitboxes = ActiveHitboxes.duplicate(true),
		_curSubaction = curSubaction.duplicate(true),
		_FightersHit = FightersHit.duplicate(true),
		_charState = charState.duplicate(true),
		_oldPose = oldPose.duplicate(true),
		
		_ledgeBox = ledgeBox,
		_stateFlags = stateFlags,
		_facing = facing,
		_grounded = grounded,
		_hitLag = hitLag,
		_hitStun = hitStun,
		_blendTime = blendTime,
		_lastStateChange = lastStateChange,
		_TransNPhysics = TransNPhysics,
		_TransNOldPos = TransNOldPos,
		_InterruptableTime = InterruptableTime,
		_jumps = jumps,
		_JustChangedState = JustChangedState,
		_downDesire = downDesire,
		_effectiveVel = effectiveVel,
		_shieldStun = shieldStun,
		_lastTimeOnLedge = lastTimeOnLedge,
		_desiredAnim = desiredAnim,
		
		animVel = animVel,
		kbVel = kbVel,
		ftVel = ftVel,
		ftPos = ftPos,
		percentage = percentage,
		internalFrameCounter = internalFrameCounter
	}
		
	if GrabbedFighter:
		state._GrabbedFighter = GrabbedFighter
	if lastHitHurtbox:
		state._lastHitHurtbox = lastHitHurtbox.duplicate(true)
	if lastHitHitbox:
		state._lastHitHitbox = lastHitHitbox.duplicate(true)
	if currentLedge:
		state._currentLedge = currentLedge.duplicate(true)
	if currentGround:
		state._currentGround = currentGround.duplicate(true)
	return state

func _load_state(state: Dictionary) -> void:
	ourShield = state["_ourShield"].duplicate(true)
	ourFightTable = state["_ourFightTable"].duplicate(true)
	ECB = state["_ECB"].duplicate(true)
	ECBOld = state["_ECBOld"].duplicate(true)
	ActiveHitboxes = state["_ActiveHitboxes"].duplicate(true)
	curSubaction = state["_curSubaction"].duplicate(true)
	FightersHit = state["_FightersHit"].duplicate(true)
	charState = state["_charState"].duplicate(true)
	oldPose = state["_oldPose"].duplicate(true)
	
	ledgeBox = state["_ledgeBox"]
	stateFlags = state["_stateFlags"]
	facing = state["_facing"]
	grounded = state["_grounded"]
	hitLag = state["_hitLag"]
	hitStun = state["_hitStun"]
	blendTime = state["_blendTime"]
	lastStateChange = state["_lastStateChange"]
	TransNPhysics = state["_TransNPhysics"]
	TransNOldPos = state["_TransNOldPos"]
	InterruptableTime = state["_InterruptableTime"]
	jumps = state["_jumps"]
	JustChangedState = state["_JustChangedState"]
	downDesire = state["_downDesire"]
	effectiveVel = state["_effectiveVel"]
	shieldStun = state["_shieldStun"]
	lastTimeOnLedge = state["_lastTimeOnLedge"]
	desiredAnim = state["_desiredAnim"]
	
	animVel = state["animVel"]
	kbVel = state["kbVel"]
	ftVel = state["ftVel"]
	ftPos = state["ftPos"]
	percentage = state["percentage"]
	internalFrameCounter = state["internalFrameCounter"]
	
		
	if state.has("_GrabbedFighter"):
		GrabbedFighter = state["_GrabbedFighter"]
	if state.has("_lastHitHurtbox"):
		lastHitHurtbox = state["_lastHitHurtbox"].duplicate(true)
	if state.has("_lastHitHitbox"):
		lastHitHitbox = state["_lastHitHitbox"].duplicate(true)
	if state.has("_currentLedge"):
		currentLedge = state["_currentLedge"].duplicate(true)
	if state.has("_currentGround"):
		currentGround = state["_currentGround"].duplicate(true)
