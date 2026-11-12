#include common_scripts\utility;
#include maps\mp\_utility;
#include maps\mp\gametypes\_hud_util;
/*
	Hide and Seek rounds, props and controls (see war.gsc).

	Dvars (set them over rcon before the map loads, or in the mod's server.cfg):
		scr_hns_hidetime    seconds the seekers wait blinded (default 30)
		scr_hns_seektime    seconds the seekers have to find every hider (default 180)
		scr_hns_minplayers  players needed to start a round (default 2, bots count)
		scr_hns_seekers     one seeker per this many players (default 4)
		scr_hns_fovscale    field of view scale for everyone (QA dvar 582, IW4 cg_fovScale; default 1.2). cg_fov
		                    stops at 90, and the client narrows third person by 0.9
		scr_hns_models      extra prop models, space separated; they must be in the client's zones

	Hider controls (CoD4 used menus here; the QA client has no script menus for this):
		Use           next prop
		Aim           previous prop
		Melee         rotate the prop 20 degrees around the current axis
		Fire          change the axis (X, -X, Y, -Y, Z)
		Smoke         lock / unlock the prop's angles while you look around
		hold Grenade  third person on / off

	QA client dvars the mod sets per player: camera_thirdPerson (its only third person switch; cg_thirdPerson does
	not exist there) and 582 (cg_fovScale). cx_mod off puts both back on the client.

	Hud text is set only to a few fixed strings: every distinct setText string takes a configstring for the rest
	of the map, so countdowns use setTimer.
*/

init()
{
	level.hns_hideTime = dvarIntDefault( "scr_hns_hidetime", 30 );
	level.hns_seekTime = dvarIntDefault( "scr_hns_seektime", 180 );
	level.hns_minPlayers = dvarIntDefault( "scr_hns_minplayers", 2 );
	level.hns_seekerRatio = dvarIntDefault( "scr_hns_seekers", 4 );
	if ( getDvar( "scr_hns_fovscale" ) == "" )
		setDvar( "scr_hns_fovscale", 1.2 );
	level.hns_fovScale = getDvarFloat( "scr_hns_fovscale" );
	if ( level.hns_seekerRatio < 1 )
		level.hns_seekerRatio = 1;

	level.hns_state = "waiting";
	level.hns_phaseEnd = 0;
	level.hns_round = 0;
	level.hns_axes = [];
	level.hns_axes[0] = "X";
	level.hns_axes[1] = "-X";
	level.hns_axes[2] = "Y";
	level.hns_axes[3] = "-Y";
	level.hns_axes[4] = "Z";

	precacheShader( "white" );
	level.hns_models = collectModels();
	level.hns_offsets = propOffsets( level.hns_models );

	level thread roundLoop();
}


dvarIntDefault( name, value )
{
	if ( getDvar( name ) == "" )
		setDvar( name, value );
	return getDvarInt( name );
}


/*
	Props. The client draws only models its zones contain, and there is no mod fastfile, so the props are the
	models the map's own script_models use (both sides have them loaded) plus scr_hns_models.
*/
collectModels()
{
	models = [];
	seen = [];

	ents = getEntArray( "script_model", "classname" );
	for ( i = 0; i < ents.size; i++ )
	{
		name = ents[i].model;
		if ( !isDefined( name ) || name == "" || isDefined( seen[name] ) || !usableModel( name ) )
			continue;
		seen[name] = true;
		models[models.size] = name;
	}

	extra = strTok( getDvar( "scr_hns_models" ), " " );
	for ( i = 0; i < extra.size; i++ )
	{
		if ( isDefined( seen[extra[i]] ) )
			continue;
		seen[extra[i]] = true;
		models[models.size] = extra[i];
	}

	// the care package crate: the killstreak scripts precache it on every map, so the client has it too
	if ( !isDefined( seen["com_plasticcase_friendly"] ) )
		models[models.size] = "com_plasticcase_friendly";

	list = "";
	for ( i = 0; i < models.size; i++ )
	{
		precacheModel( models[i] );
		list = list + " " + models[i];
	}
	// readable over rcon: cx_rcon scr_hns_props
	setDvar( "scr_hns_props", models.size + ":" + list );
	codolPrintLn( "hns: " + models.size + " props on " + getDvar( "mapname" ) + ":" + list );
	return models;
}


/*
	Where each prop's origin sits relative to its bottom centre. Map props are authored for their own spot (wall
	boxes and laundry hang from their origin), so placed on a hider's feet they sink into floors and walls; the
	offset stands them on the ground, centred on the hider. Measured once on a probe entity at the world origin.
*/
propOffsets( models )
{
	offsets = [];
	probe = spawn( "script_model", ( 0, 0, 0 ) );
	probe.angles = ( 0, 0, 0 );
	list = "";
	for ( i = 0; i < models.size; i++ )
	{
		probe setModel( models[i] );
		bottom = probe getPointInBounds( 0, 0, -1 );
		offsets[models[i]] = ( 0, 0, 0 ) - bottom;
		list = list + " " + models[i] + "=" + offsets[models[i]];
	}
	probe delete();
	// readable over rcon: cx_rcon scr_hns_offsets
	setDvar( "scr_hns_offsets", list );
	return offsets;
}


usableModel( name )
{
	name = toLower( name );
	// helpers, gear and set dressing that makes no sense as a prop (seatown: wire bundles, the C-130 intro rig)
	skip = strTok( "tag_origin collision clip fx_ weapon_ viewmodel viewhands projectile body_ head_ _bombsquad wire zoomrig c130 ac130", " " );
	for ( i = 0; i < skip.size; i++ )
	{
		if ( isSubStr( name, skip[i] ) )
			return false;
	}
	return true;
}


/*
	Rounds
*/
roundLoop()
{
	level endon( "game_ended" );

	level waittill( "prematch_over" );

	for ( ;; )
	{
		setPhase( "waiting", 0 );
		while ( playingPlayers().size < level.hns_minPlayers )
		{
			setStatusAll( "Hide and Seek: waiting for more players" );
			wait 1;
		}

		winner = playRound();
		endRound( winner );
		wait 6;
	}
}


playingPlayers()
{
	result = [];
	for ( i = 0; i < level.players.size; i++ )
	{
		player = level.players[i];
		if ( isDefined( player.pers["team"] ) && ( player.pers["team"] == "allies" || player.pers["team"] == "axis" ) )
			result[result.size] = player;
	}
	return result;
}


playRound()
{
	level.hns_round++;
	players = playingPlayers();

	seekers = int( players.size / level.hns_seekerRatio );
	if ( seekers < 1 )
		seekers = 1;

	for ( i = 0; i < players.size; i++ )
		players[i].hns_hider = true;
	for ( picked = 0; picked < seekers; )
	{
		player = players[randomInt( players.size )];
		if ( !player.hns_hider )
			continue;
		player.hns_hider = false;
		picked++;
	}

	setPhase( "hiding", level.hns_hideTime );
	codolPrintLn( "hns: round " + level.hns_round + ", " + players.size + " players, " + seekers + " seeker(s)" );

	// everyone starts the round fresh, on the team the draw gave them
	for ( i = 0; i < players.size; i++ )
	{
		player = players[i];
		if ( isAlive( player ) )
		{
			// the next life's stow thread starts with nothing attached, so take this life's models off
			player maps\mp\gametypes\_weapons::detach_all_weapons();
			player thread maps\mp\gametypes\_playerlogic::spawnPlayer();
		}
		else
			player applyRoundTeam();
	}

	setStatusTeam( "allies", "Hide! The seekers are released in" );
	setStatusTeam( "axis", "The hiders are hiding" );
	wait level.hns_hideTime;

	setPhase( "seeking", level.hns_seekTime );
	level notify( "hns_seek" );
	iPrintLnBold( "The seekers are out!" );
	setStatusTeam( "allies", "Stay hidden" );
	setStatusTeam( "axis", "Find the hiders" );

	lastCount = -1;
	for ( left = level.hns_seekTime * 4; left > 0; left-- )
	{
		hiders = countHiders();
		if ( !hiders )
			return "axis";
		if ( hiders != lastCount )
		{
			lastCount = hiders;
			iPrintLn( "Hiders left: " + hiders );
		}
		wait 0.25;
	}
	if ( !countHiders() )
		return "axis";
	return "allies";
}


// state and its end time; players who spawn later read the countdown from here
setPhase( state, seconds )
{
	level.hns_state = state;
	level.hns_phaseEnd = getTime() + seconds * 1000;
	for ( i = 0; i < level.players.size; i++ )
		level.players[i] showTimer();
}


countHiders()
{
	count = 0;
	for ( i = 0; i < level.players.size; i++ )
	{
		player = level.players[i];
		if ( isDefined( player.hns_hider ) && player.hns_hider && player.pers["team"] == "allies" )
			count++;
	}
	return count;
}


endRound( winner )
{
	setPhase( "ended", 0 );
	maps\mp\gametypes\_gamescore::giveTeamScoreForObjective( winner, 1 );

	if ( winner == "axis" )
		text = "The seekers found everyone";
	else
		text = "The hiders survived";
	iPrintLnBold( text );
	setStatusAll( text );
	codolPrintLn( "hns: round " + level.hns_round + ": " + text );
}


/*
	Teams. The draw is kept on the player (hns_hider) and applied whenever they spawn.
*/
applyRoundTeam()
{
	if ( level.hns_state == "waiting" )
		return;
	if ( !isDefined( self.pers["team"] ) || self.pers["team"] == "spectator" )
		return;

	// a player who joins during a round seeks
	if ( !isDefined( self.hns_hider ) || level.hns_state == "ended" )
		self.hns_hider = false;

	if ( self.hns_hider )
		team = "allies";
	else
		team = "axis";
	if ( self.pers["team"] == team )
		return;

	self maps\mp\gametypes\_menus::addToTeam( team );
	self.sessionteam = team;
}


onSpawnPlayer()
{
	self thread setupSpawn();
}


setupSpawn()
{
	self notify( "hns_spawn" );
	self endon( "hns_spawn" );
	self endon( "disconnect" );
	self endon( "death" );

	self waittill( "spawned_player" );

	self removeProp();
	self removeBlind();
	self showTimer();

	if ( self.pers["team"] == "allies" && level.hns_state != "waiting" )
		self becomeHider();
	else
		self becomeSeeker();
}


onPlayerKilled( eInflictor, attacker, iDamage, sMeansOfDeath, sWeapon, vDir, sHitLoc, psOffsetTime, deathAnimDuration, lifeId )
{
	self removeProp();
	self removeBlind();

	if ( !isDefined( self.hns_hider ) || !self.hns_hider )
		return;
	if ( level.hns_state != "hiding" && level.hns_state != "seeking" )
		return;

	self.hns_hider = false;
	if ( isPlayer( attacker ) && attacker != self )
		iPrintLn( attacker.name + " found " + self.name );
	else
		iPrintLn( self.name + " is out" );
	codolPrintLn( "hns: " + self.name + " caught, " + countHiders() + " hider(s) left" );
}


becomeSeeker()
{
	self show();
	self setClientDvars( "camera_thirdPerson", 0, "582", level.hns_fovScale, "cg_drawCrosshairNames", 0 );

	if ( level.hns_state == "hiding" )
		self thread blindUntilSeek();

	if ( level.hns_state == "waiting" )
		self setStatus( "Hide and Seek: waiting for more players" );
	else
		self setStatus( "You are a seeker" );
}


blindUntilSeek()
{
	self endon( "disconnect" );
	self endon( "death" );
	self endon( "hns_spawn" );

	self freezeControls( true );
	blind = newClientHudElem( self );
	blind.x = 0;
	blind.y = 0;
	blind.horzAlign = "fullscreen";
	blind.vertAlign = "fullscreen";
	blind.alignX = "left";
	blind.alignY = "top";
	blind.sort = -2;
	blind.color = ( 0, 0, 0 );
	blind.alpha = 1;
	blind setShader( "white", 640, 480 );
	self.hns_blind = blind;

	level waittill( "hns_seek" );
	self removeBlind();
}


removeBlind()
{
	if ( isDefined( self.hns_blind ) )
	{
		self.hns_blind destroy();
		self.hns_blind = undefined;
	}
	self freezeControls( false );
}


becomeHider()
{
	self takeAllWeapons();
	self hide();
	self setClientDvars( "camera_thirdPerson", 1, "582", level.hns_fovScale );
	self.hns_third = true;
	self.hns_axis = 4;
	self.hns_rot = ( 0, 0, 0 );
	self.hns_locked = false;
	self.hns_propIndex = randomInt( level.hns_models.size );

	prop = spawn( "script_model", self.origin );
	prop setModel( level.hns_models[self.hns_propIndex] );
	prop setCanDamage( true );
	self.hns_prop = prop;
	self placeProp();

	prop thread propDamage( self );
	self thread propCleanup( prop );
	self thread hiderControls();
	self thread lockedProp();

	self setStatus( "You are a hider" );
	self iPrintLn( "Prop: " + level.hns_models[self.hns_propIndex] );
	self iPrintLn( "Use / Aim: change prop   Melee: rotate   Fire: axis   Smoke: lock   hold Grenade: camera" );
}


removeProp()
{
	self notify( "hns_noprop" );
	if ( isDefined( self.hns_prop ) )
	{
		self.hns_prop delete();
		self.hns_prop = undefined;
	}
	self show();
}


// the prop must not outlive its hider
propCleanup( prop )
{
	self waittill_any( "disconnect", "death", "hns_spawn", "hns_noprop" );
	if ( isDefined( prop ) )
		prop delete();
}


// put the prop on the hider with the rotation chosen so far, bottom centre on the hider's feet; the link makes it
// follow position and yaw
placeProp()
{
	prop = self.hns_prop;
	if ( !isDefined( prop ) )
		return;
	prop unlink();
	if ( self.hns_locked )
		angles = self.hns_lockAngles;
	else
		angles = ( self.hns_rot[0], self.angles[1] + self.hns_rot[1], self.hns_rot[2] );
	offset = level.hns_offsets[level.hns_models[self.hns_propIndex]];
	prop.angles = angles;
	prop.origin = self.origin + anglesToForward( angles ) * offset[0] - anglesToRight( angles ) * offset[1] + anglesToUp( angles ) * offset[2];
	prop linkTo( self );
}


// a locked prop keeps its world angles while the hider looks around (CoD4 "fix")
lockedProp()
{
	self endon( "disconnect" );
	self endon( "death" );
	self endon( "hns_spawn" );
	self endon( "hns_noprop" );

	for ( ;; )
	{
		wait 0.05;
		if ( self.hns_locked )
			self placeProp();
	}
}


setPropModel( step )
{
	count = level.hns_models.size;
	self.hns_propIndex = ( self.hns_propIndex + step + count ) % count;
	self.hns_prop setModel( level.hns_models[self.hns_propIndex] );
	self placeProp();
	self iPrintLn( "Prop: " + level.hns_models[self.hns_propIndex] );
}


rotateProp()
{
	switch ( level.hns_axes[self.hns_axis] )
	{
		case "X":
			delta = ( 20, 0, 0 );
			break;
		case "-X":
			delta = ( -20, 0, 0 );
			break;
		case "Y":
			delta = ( 0, 0, 20 );
			break;
		case "-Y":
			delta = ( 0, 0, -20 );
			break;
		default:
			delta = ( 0, 20, 0 );
			break;
	}
	self.hns_rot = self.hns_rot + delta;
	if ( self.hns_locked )
		self.hns_lockAngles = self.hns_lockAngles + delta;
	self placeProp();
}


hiderControls()
{
	self endon( "disconnect" );
	self endon( "death" );
	self endon( "hns_spawn" );
	self endon( "hns_noprop" );

	for ( ;; )
	{
		wait 0.05;
		if ( self useButtonPressed() )
		{
			self setPropModel( 1 );
			self waitRelease( "use" );
		}
		else if ( self adsButtonPressed() )
		{
			self setPropModel( -1 );
			self waitRelease( "ads" );
		}
		else if ( self meleeButtonPressed() )
		{
			self rotateProp();
			self waitRelease( "melee" );
		}
		else if ( self attackButtonPressed() )
		{
			self.hns_axis = ( self.hns_axis + 1 ) % level.hns_axes.size;
			self iPrintLn( "Rotation axis: " + level.hns_axes[self.hns_axis] );
			self waitRelease( "attack" );
		}
		else if ( self secondaryOffhandButtonPressed() )
		{
			self.hns_locked = !self.hns_locked;
			if ( self.hns_locked )
			{
				self.hns_lockAngles = self.hns_prop.angles;
				self iPrintLn( "Prop locked" );
			}
			else
			{
				self iPrintLn( "Prop follows your view" );
			}
			self placeProp();
			self waitRelease( "smoke" );
		}
		else if ( self fragButtonPressed() )
		{
			held = 0;
			while ( self fragButtonPressed() && held < 0.5 )
			{
				wait 0.05;
				held += 0.05;
			}
			if ( held >= 0.5 )
			{
				self.hns_third = !self.hns_third;
				self setClientDvar( "camera_thirdPerson", self.hns_third );
			}
			self waitRelease( "frag" );
		}
	}
}


waitRelease( button )
{
	for ( ;; )
	{
		switch ( button )
		{
			case "use":
				pressed = self useButtonPressed();
				break;
			case "ads":
				pressed = self adsButtonPressed();
				break;
			case "melee":
				pressed = self meleeButtonPressed();
				break;
			case "attack":
				pressed = self attackButtonPressed();
				break;
			case "smoke":
				pressed = self secondaryOffhandButtonPressed();
				break;
			default:
				pressed = self fragButtonPressed();
				break;
		}
		if ( !pressed )
			return;
		wait 0.05;
	}
}


// a shot at the prop hurts its hider like a shot at the hider (the hidden player keeps its hitboxes too)
propDamage( owner )
{
	self endon( "death" );
	owner endon( "disconnect" );
	owner endon( "hns_noprop" );

	for ( ;; )
	{
		self waittill( "damage", amount, attacker, dir, point, mod );
		if ( !isAlive( owner ) || !isPlayer( attacker ) || attacker == owner )
			continue;
		if ( !isDefined( attacker.pers["team"] ) || attacker.pers["team"] == owner.pers["team"] )
			continue;
		owner thread [[level.callbackPlayerDamage]]( attacker, attacker, amount, 0, mod, attacker getCurrentWeapon(), point, dir, "torso_upper", 0 );
	}
}


/*
	Hud: a status line and a countdown at the top of the screen, one of each per player
*/
setStatus( text )
{
	if ( !isDefined( self.hns_status ) )
	{
		status = self createFontString( "default", 1.5 );
		status setPoint( "TOP", "TOP", 0, 32 );
		status.sort = 10;
		self.hns_status = status;
	}
	if ( !isDefined( self.hns_statusText ) || self.hns_statusText != text )
	{
		self.hns_statusText = text;
		self.hns_status setText( text );
	}
}


showTimer()
{
	if ( !isDefined( self.hns_timer ) )
	{
		timer = self createTimer( "default", 1.5 );
		timer setPoint( "TOP", "TOP", 0, 52 );
		timer.sort = 10;
		self.hns_timer = timer;
	}
	left = int( ( level.hns_phaseEnd - getTime() ) / 1000 );
	if ( left > 0 && ( level.hns_state == "hiding" || level.hns_state == "seeking" ) )
	{
		self.hns_timer.alpha = 1;
		self.hns_timer setTimer( left );
	}
	else
	{
		self.hns_timer.alpha = 0;
	}
}


setStatusAll( text )
{
	for ( i = 0; i < level.players.size; i++ )
		level.players[i] setStatus( text );
}


setStatusTeam( team, text )
{
	for ( i = 0; i < level.players.size; i++ )
	{
		if ( level.players[i].pers["team"] == team )
			level.players[i] setStatus( text );
	}
}
