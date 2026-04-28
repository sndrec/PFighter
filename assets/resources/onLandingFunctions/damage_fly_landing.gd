class_name DamageFlyLanding extends LandingFunc

func _execute(inFt: Fighter) -> void:
	inFt.jumps = 0
	inFt.set_fighter_flag(12, false)
	inFt._change_fighter_state(inFt.find_state_by_name("DownBound"), 4, 0)
