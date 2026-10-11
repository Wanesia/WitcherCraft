// WitcherCraft M2b: Minecraft blocks in The Witcher 3, and build mode.
//
// Driven from CWCraftManager.Step (witchercraft.ws), ~30 Hz:
//   - build mode: F7 (read natively, WCraft_BuildMode) blocks Geralt's combat/interaction
//     actions while on; the native host forwards LMB / RMB / MMB / wheel / 1-9 / E to Minecraft
//   - aim: camera ray -> StaticTrace -> WCraft_SetAim (Minecraft looks from its eye at that point)
//   - blocks: Minecraft's blocks exist in Minecraft only. WCraft_PollBlocks deltas are drained
//     (the native side keeps its block list for build-mode aim) but nothing is spawned in The
//     Witcher: an invisible collision box per block was tried, and as Geralt it was an invisible
//     wall (the blocks aren't drawn in Witcher mode); NPCs walk by the navigation mesh and went
//     through it anyway, and a navigation no-go area per block was ignored by the navigation.
//     AddBlock still knows how to spawn a box (boxPath) if that is ever wanted again.
// Console: wcbuild()  wcblocks()  wcmc("gamemode survival")
//
// Natives (witchercraft_natives.ws): WCraft_PollBlocks, WCraft_ClearBlocks, WCraft_BuildMode,
// WCraft_SetBuildMode, WCraft_SetAim, WCraft_McCommand, WCraft_BlockStatus.

class CWCraftBlocks
{
	// A quest's invisible collision box: 1 m, pivot in its centre, hidden mesh (left hidden).
	private var boxPath : string;
	private var box : CEntityTemplate;
	private var ents : array< CEntity >;     // indexed by the native block id
	private var spawned : int;
	private var capLogged : bool;
	private var buildOn : bool;
	private var groups : array< name >;
	private var lastAimLog : float;
	private var maxSpawned : int;
	default maxSpawned = 2000;

	public function Init()
	{
		var boxOk : bool;

		if ( groups.Size() == 0 )
		{
			boxPath = "dlc\bob\data\quests\minor_quests\quest_files\mq7002_stubborn_knight\entities\mq7002_collision.w2ent";
			groups.PushBack( 'Terrain' );
			groups.PushBack( 'Static' );
			groups.PushBack( 'Destructible' );
			groups.PushBack( 'Platforms' );
			groups.PushBack( 'Boat' );
			groups.PushBack( 'Door' );
		}
		// No Witcher-side object per block (see the top of this file): box stays NULL, AddBlock skips.
		box = NULL;
		boxOk = false;
		// A (re)started manager has no entities: the native side announces every block again.
		DestroyAll();
		WCraft_ClearBlocks();
		buildOn = false;
		WCraft_ControlNote( "blocks: ready (no Witcher-side collision for Minecraft blocks, box " + boxOk + ")" );
	}

	public function OnWorldChanged()
	{
		DestroyAll();
		WCraft_ClearBlocks();
	}

	public function Step()
	{
		UpdateBuildMode();
		if ( buildOn )
		{
			Aim();
		}
		ApplyDeltas( 48 );
	}

	// ---- build mode --------------------------------------------------------------------------
	private function UpdateBuildMode()
	{
		var on : bool;

		on = WCraft_BuildMode();
		if ( on == buildOn )
		{
			return;
		}
		buildOn = on;
		SetActionsBlocked( on );
		if ( on )
		{
			theGame.GetGuiManager().ShowNotification( "WitcherCraft build mode ON (F7 to leave)<br>LMB break, RMB place, wheel / 1-9 pick block", 4.0 );
		}
		else
		{
			WCraft_SetAim( false, Vector( 0, 0, 0 ), Vector( 0, 0, 0 ), Vector( 0, 0, 0, 0 ) );
			theGame.GetGuiManager().ShowNotification( "WitcherCraft build mode OFF", 2.0 );
		}
		WCraft_ControlNote( "build mode " + on );
	}

	private function SetActionsBlocked( on : bool )
	{
		var actions : array< EInputActionBlock >;
		var i : int;

		actions.PushBack( EIAB_LightAttacks );
		actions.PushBack( EIAB_HeavyAttacks );
		actions.PushBack( EIAB_SpecialAttackLight );
		actions.PushBack( EIAB_SpecialAttackHeavy );
		actions.PushBack( EIAB_SwordAttack );
		actions.PushBack( EIAB_Fists );
		actions.PushBack( EIAB_Signs );
		actions.PushBack( EIAB_DrawWeapon );
		actions.PushBack( EIAB_ThrowBomb );
		actions.PushBack( EIAB_Crossbow );
		actions.PushBack( EIAB_UsableItem );
		actions.PushBack( EIAB_QuickSlots );
		actions.PushBack( EIAB_ExplorationFocus );   // RMB = Witcher senses
		actions.PushBack( EIAB_Interactions );       // E
		actions.PushBack( EIAB_InteractionAction );
		actions.PushBack( EIAB_InteractionContainers );
		for ( i = 0; i < actions.Size(); i += 1 )
		{
			if ( on )
			{
				thePlayer.BlockAction( actions[ i ], 'WitcherCraftBuild' );
			}
			else
			{
				thePlayer.UnblockAction( actions[ i ], 'WitcherCraftBuild' );
			}
		}
	}

	// Camera ray -> what the player aims at; Minecraft looks at the same point from its own eye.
	private function Aim()
	{
		var cam : Vector;
		var dir : Vector;
		var far : Vector;
		var hit : Vector;
		var normal : Vector;
		var target : Vector;
		var now : float;

		cam = theCamera.GetCameraPosition();
		dir = theCamera.GetCameraDirection();
		far = cam + dir * 12.0;
		if ( theGame.GetWorld().StaticTrace( cam, far, hit, normal, groups ) )
		{
			hit.W = 1.0;
		}
		else
		{
			hit = Vector( 0, 0, 0, 0 );
		}
		// The native side also walks the shown Minecraft blocks along the ray (nearest wins).
		target = WCraft_SetAim( true, cam, dir, hit );
		now = theGame.GetEngineTimeAsSeconds();
		if ( now - lastAimLog > 5.0 )
		{
			lastAimLog = now;
			LogChannel( 'WitcherCraft', "aim " + VecToStringPrec( target, 2 ) + " (cam " + VecToStringPrec( cam, 2 ) + ")" );
		}
	}

	// ---- block entities ---------------------------------------------------------------------
	private function ApplyDeltas( max : int )
	{
		var data : string;
		var item : string;
		var rest : string;
		var guard : int;

		data = WCraft_PollBlocks( max );
		guard = 0;
		while ( StrLen( data ) > 0 && guard < 300 )
		{
			guard += 1;
			if ( !StrSplitFirst( data, ";", item, rest ) )
			{
				item = data;
				rest = "";
			}
			data = rest;
			if ( StrLen( item ) < 2 )
			{
				continue;
			}
			if ( StrLeft( item, 1 ) == "+" )
			{
				AddBlock( StrMid( item, 1 ) );
			}
			else if ( StrLeft( item, 1 ) == "-" )
			{
				RemoveBlock( StringToInt( StrMid( item, 1 ), -1 ) );
			}
		}
	}

	// "id,x,y,z,class" (W3 min corner of the block).
	private function AddBlock( s : string )
	{
		var f : array< int >;
		var left : string;
		var rest : string;
		var i : int;
		var id : int;
		var pos : Vector;
		var ent : CEntity;

		rest = s;
		for ( i = 0; i < 4; i += 1 )
		{
			if ( !StrSplitFirst( rest, ",", left, rest ) )
			{
				return;
			}
			f.PushBack( StringToInt( left, 0 ) );
		}
		f.PushBack( StringToInt( rest, 0 ) );
		id = f[ 0 ]; // f[ 4 ], the colour class, doesn't matter: every block is the same invisible box
		if ( id < 0 || !box )
		{
			return;
		}
		while ( ents.Size() <= id )
		{
			ents.PushBack( NULL );
		}
		if ( ents[ id ] )
		{
			RemoveBlock( id ); // shouldn't happen: ids are freed before reuse
		}
		if ( spawned >= maxSpawned )
		{
			if ( !capLogged )
			{
				capLogged = true;
				WCraft_ControlNote( "blocks: entity cap " + maxSpawned + " reached" );
			}
			return;
		}
		// Block occupies [x, x+1] x [y, y+1] x [z, z+1] (W3 metres); the box's pivot is its centre.
		pos = Vector( (float)f[ 1 ] + 0.5, (float)f[ 2 ] + 0.5, (float)f[ 3 ] + 0.5 );
		ent = theGame.CreateEntity( box, pos, EulerAngles( 0, 0, 0 ), false, false, true, PM_DontPersist );
		if ( !ent )
		{
			WCraft_ControlNote( "blocks: CreateEntity failed" );
			return;
		}
		ents[ id ] = ent;
		spawned += 1;
	}

	private function RemoveBlock( id : int )
	{
		if ( id < 0 || id >= ents.Size() )
		{
			return;
		}
		if ( ents[ id ] )
		{
			ents[ id ].Destroy();
			ents[ id ] = NULL;
			spawned -= 1;
		}
	}

	public function DestroyAll()
	{
		var i : int;

		for ( i = 0; i < ents.Size(); i += 1 )
		{
			if ( ents[ i ] )
			{
				ents[ i ].Destroy();
			}
		}
		ents.Clear();
		spawned = 0;
		capLogged = false;
	}

	public function SpawnedCount() : int
	{
		return spawned;
	}
}

function WCraft_Blocks() : CWCraftBlocks
{
	var mgr : CWCraftManager;

	mgr = WCraft_Manager();
	if ( mgr )
	{
		return mgr.GetBlocks();
	}
	return NULL;
}

exec function wcbuild()
{
	WCraft_SetBuildMode( !WCraft_BuildMode() );
}

exec function wcblocks()
{
	var msg : string;
	var b : CWCraftBlocks;

	msg = WCraft_BlockStatus();
	b = WCraft_Blocks();
	if ( b )
	{
		msg += "<br>entities " + b.SpawnedCount();
	}
	LogChannel( 'WitcherCraft', msg );
	theGame.GetGuiManager().ShowNotification( msg, 6.0 );
}

exec function wcmc( command : string )
{
	WCraft_McCommand( command );
}
