class_name DownEnter extends FighterFunction

func _execute(inFt: Fighter) -> void:
	var desiredAnim = inFt.charState.stateName + inFt.downDesire
	inFt.desiredAnim = desiredAnim
	inFt.Animator.current_animation = inFt.desiredAnim
	inFt.Animator.assigned_animation = inFt.desiredAnim
	inFt.Animator.seek(0, true)
	inFt.update_pose()
