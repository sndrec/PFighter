class_name EnteredNewState extends FighterFunction

func _execute(inFt: Fighter):
	inFt.clear_hitboxes()
	inFt.unfold_action(inFt.charState.action)
	inFt.FighterSkeleton.clear_bones_global_pose_override()
	inFt.FighterSkeleton.reset_bone_poses()
	inFt.ourShield.active = false
	inFt.get_node("ShieldSprite").visible = false
	var hasAnim = inFt.charState.stateAnim != ""
	if inFt.facing == -1:
		inFt.rotation = Vector3(0, PI, 0)
	else:
		inFt.rotation = Vector3.ZERO
	
	if hasAnim:
		inFt.desiredAnim = inFt.charState.stateAnim
		inFt.Animator.current_animation = inFt.desiredAnim
		inFt.Animator.assigned_animation = inFt.desiredAnim
		inFt.Animator.seek(0, true)
		inFt.update_pose()
