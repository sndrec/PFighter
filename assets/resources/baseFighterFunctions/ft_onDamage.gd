class_name StandardOnDamage extends OnDamage

var DownStates: Dictionary = {
	"DownBound" = true,
	"DownWait" = true,
	"DownDamage" = true
}

func _execute(inFt: Fighter, inHurtbox: HurtboxDefinition, inHitbox: HitboxDefinition, attacker: Fighter) -> void:
	var invincible = inHurtbox.bodyState == HurtboxDefinition.hurtboxBodyState.Invincible
	if !invincible:
		inFt.percentage += inHitbox.damage
	var knockBack = FHelp.CalculateKnockback(inHitbox, attacker, inFt)
	# tumble threshold is 80 knockback!
	# 
	var side = sign(inFt.ftPos.x - attacker.ftPos.x)
	var kbAngle = Vector2.from_angle(inHitbox.kbAngle)
	kbAngle.x *= attacker.facing
	var kbVelMult = 1.0
	if !inHitbox.kbAngleFixed and side != attacker.facing:
		kbAngle.x *= -1
	var oldFacing = inFt.facing
	if !invincible:
		inFt.facing = -side
		inFt.ftVel = Vector2.ZERO
		inFt.kbVel = kbAngle * knockBack * 0.03 * kbVelMult
		inFt.hitStun = floor(knockBack * 0.4)
	var c = 1
	var e = 1
	var ourHitlag = floor(c * floor(e * floor(3+inHitbox.damage/3)))
	if !invincible:
		inFt.hitLag = ourHitlag
	attacker.hitLag = ourHitlag
	#print("Knockback: " + str(knockBack))
	#print("Hitstun: " + str(inFt.hitStun))
	#print("Hitlag: " + str(ourHitlag))
	if invincible:
		return
	inFt.desiredAnim = "DamageN1"
	if knockBack < 80:
		if DownStates.has(inFt.charState.stateName) and knockBack <= 25:
			inFt.facing = oldFacing
			inFt._change_fighter_state(inFt.find_state_by_name("DownDamage"), 0, 0)
			inFt.desiredAnim = "DownDamage" + inFt.downDesire
		else:
			inFt._change_fighter_state(inFt.find_state_by_name("Damage"), 0, 0)
			if inFt.grounded:
				if knockBack < 27:
					if inHurtbox.bodyType == HurtboxDefinition.hurtboxBodyType.High:
						inFt.desiredAnim = "DamageHi1"
					else: if inHurtbox.bodyType == HurtboxDefinition.hurtboxBodyType.Middle:
						inFt.desiredAnim = "DamageN1"
					else:
						inFt.desiredAnim = "DamageLw1"
				else: if knockBack < 54:
					if inHurtbox.bodyType == HurtboxDefinition.hurtboxBodyType.High:
						inFt.desiredAnim = "DamageHi2"
					else: if inHurtbox.bodyType == HurtboxDefinition.hurtboxBodyType.Middle:
						inFt.desiredAnim = "DamageN2"
					else:
						inFt.desiredAnim = "DamageLw2"
				else:
					if inHurtbox.bodyType == HurtboxDefinition.hurtboxBodyType.High:
						inFt.desiredAnim = "DamageHi3"
					else: if inHurtbox.bodyType == HurtboxDefinition.hurtboxBodyType.Middle:
						inFt.desiredAnim = "DamageN3"
					else:
						inFt.desiredAnim = "DamageLw3"
			else:
				if knockBack < 27:
					inFt.desiredAnim = "DamageAir1"
				else: if knockBack < 54:
					inFt.desiredAnim = "DamageAir2"
				else:
					inFt.desiredAnim = "DamageAir3"
	else:
		inFt._change_fighter_state(inFt.find_state_by_name("DamageFly"), 0, 0)
		if inFt.grounded and inFt.kbVel.y < 0:
			#print("Started grounded, and spiked - let's invert the y.")
			inFt.kbVel.y *= -0.8
		if inHurtbox.bodyType == HurtboxDefinition.hurtboxBodyType.High:
			inFt.desiredAnim = "DamageFlyHi"
		else: if inHurtbox.bodyType == HurtboxDefinition.hurtboxBodyType.Middle:
			inFt.desiredAnim = "DamageFlyN"
		else:
			inFt.desiredAnim = "DamageFlyLw"
		if inHitbox.kbAngle > PI * 0.333 and inHitbox.kbAngle < PI * 0.666:
			inFt.desiredAnim = "DamageFlyTop"
	inFt.Animator.current_animation = inFt.desiredAnim
	inFt.Animator.assigned_animation = inFt.desiredAnim
	inFt.Animator.seek(1.0 / 60.0, true)
	inFt.update_pose()
	inFt.check_on_airborne()
	
	inFt.badgeGrid.update_player_percent(inFt)
