#include maps\mp\_utility;
#include maps\mp\gametypes\_hud_util;
/*
	Hide and Seek (prop hunt), loaded in place of Team Deathmatch by r1-mod's cx_mod hns.

	Hiders (allies) become props of the current map, seekers (axis) wait blinded for the hide time and then
	hunt them. A hider that dies joins the seekers. The round ends when no hider is left (seekers score) or
	when the seek time runs out (hiders score), then the teams are drawn again. The game logic is in _hns.gsc.

	Port of the core round of iCore's Hide 'n Seek for CoD4 (github.com/KristofMorva/hns, MIT licence).
*/

main()
{
	if(getdvar("mapname") == "mp_background")
		return;

	maps\mp\gametypes\_globallogic::init();
	maps\mp\gametypes\_callbacksetup::SetupCallbacks();
	maps\mp\gametypes\_globallogic::SetupCallbacks();

	registerTimeLimitDvar( level.gameType, 0, 0, 1440 );
	registerScoreLimitDvar( level.gameType, 0, 0, 100 );
	registerRoundLimitDvar( level.gameType, 1, 0, 10 );
	registerWinLimitDvar( level.gameType, 1, 0, 10 );
	registerRoundSwitchDvar( level.gameType, 0, 0, 30 );
	registerNumLivesDvar( level.gameType, 0, 0, 10 );
	registerHalfTimeDvar( level.gameType, 0, 0, 1 );

	level.teamBased = true;
	level.onStartGameType = ::onStartGameType;
	level.getSpawnPoint = ::getSpawnPoint;
	level.onSpawnPlayer = maps\mp\gametypes\_hns::onSpawnPlayer;
	level.onPlayerKilled = maps\mp\gametypes\_hns::onPlayerKilled;

	game["dialog"]["gametype"] = "tm_death";
}


onStartGameType()
{
	setClientNameMode("auto_change");

	if ( !isdefined( game["switchedsides"] ) )
		game["switchedsides"] = false;

	setObjectiveText( "allies", &"OBJECTIVES_WAR" );
	setObjectiveText( "axis", &"OBJECTIVES_WAR" );
	setObjectiveScoreText( "allies", &"OBJECTIVES_WAR_SCORE" );
	setObjectiveScoreText( "axis", &"OBJECTIVES_WAR_SCORE" );
	setObjectiveHintText( "allies", &"OBJECTIVES_WAR_HINT" );
	setObjectiveHintText( "axis", &"OBJECTIVES_WAR_HINT" );

	level.spawnMins = ( 0, 0, 0 );
	level.spawnMaxs = ( 0, 0, 0 );
	maps\mp\gametypes\_spawnlogic::placeSpawnPoints( "mp_tdm_spawn_allies_start" );
	maps\mp\gametypes\_spawnlogic::placeSpawnPoints( "mp_tdm_spawn_axis_start" );
	maps\mp\gametypes\_spawnlogic::addSpawnPoints( "allies", "mp_tdm_spawn" );
	maps\mp\gametypes\_spawnlogic::addSpawnPoints( "axis", "mp_tdm_spawn" );

	level.mapCenter = maps\mp\gametypes\_spawnlogic::findBoxCenter( level.spawnMins, level.spawnMaxs );
	setMapCenter( level.mapCenter );

	allowed[0] = level.gameType;
	maps\mp\gametypes\_gameobjects::main(allowed);

	// UAVs and the rest would give hiders away
	level.killstreakRewards = false;

	maps\mp\gametypes\_hns::init();
}


getSpawnPoint()
{
	// the round decides the team; spawnPlayer reads it right after this
	self maps\mp\gametypes\_hns::applyRoundTeam();

	spawnPoints = maps\mp\gametypes\_spawnlogic::getTeamSpawnPoints( self.pers["team"] );
	return maps\mp\gametypes\_spawnlogic::getSpawnpoint_NearTeam( spawnPoints );
}
