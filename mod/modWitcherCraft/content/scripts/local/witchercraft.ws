// WitcherCraft M1 ("link up", mirror mode): Geralt drives, a hidden SkyCraft Minecraft follows.
// M3a: by default MINECRAFT drives (witchercraft_control.ws); F8 switches back
// to this mirror mode ("Witcher mode"). Ground is then sampled around Minecraft's player
// (WCraft_SampleCentre) instead of Geralt.
//
// Every ~33 ms (latent loop in a state machine, like vanilla's PlayerAiming):
//   - WCraft_HostTick: Geralt's position / heading / camera pitch -> SkyState (native host thread)
//   - ground heights in a 33x33 m area around Geralt (StaticTrace straight down, 96 per tick,
//     ring order) -> WCraft_PushHeights -> collision height field for Minecraft
//   - every 10 s: an on-screen line with WCraft_GetMcStatus() (toggle: wcdebug(false))
// M4a: CWCraftCombat (witchercraft_combat.ws) runs in the same step: W3 actors -> Minecraft stand-ins,
// Minecraft hits -> W3 damage, W3 hits on the hidden player -> Minecraft.
// Console: wcstatus()  wcdebug(true|false)  wcraft()  (selftest, witchercraft_natives.ws)  wccombat()
//
// Natives are registered by dinput8.dll (native\src\natives.cpp).

import function WCraft_HostTick( pos : Vector, headingDeg : float, pitchDeg : float, flags : int ) : bool;
import function WCraft_PushHeightColumn( x : int, y : int, groundZ : float );
import function WCraft_PushHeights( data : string ) : int;
import function WCraft_SetWorld( worldName : string );
import function WCraft_GetMcStatus() : string;
import function WCraft_GetMcPos() : Vector;
import function WCraft_PollBlocks( max : int ) : string;
import function WCraft_ClearBlocks();
import function WCraft_BuildMode() : bool;
import function WCraft_SetBuildMode( on : bool );
import function WCraft_SetAim( active : bool, camPos : Vector, camDir : Vector, hit : Vector ) : Vector;
import function WCraft_McCommand( command : string );
import function WCraft_BlockStatus() : string;
// M3a Minecraft mode (witchercraft_control.ws, native\src\natives_control.cpp)
import function WCraft_ControlFrame( flags : int, camHeading : float, camPitch : float ) : Vector;
import function WCraft_ControlLook() : Vector;
import function WCraft_ControlFeet() : Vector;
import function WCraft_ControlMode() : bool;
import function WCraft_SetControlMode( on : bool );
import function WCraft_ControlNote( msg : string );
import function WCraft_SampleCentre() : Vector;
import function WCraft_CameraOption( fovMode : int, usePose : int ) : string;
// M5a: W3 walls and buildings as Minecraft collision (native\src\collision_physx.cpp)
import function WCraft_ColScan() : int;
import function WCraft_ColStatus() : string;
import function WCraft_ColEnable( on : bool );

// Flags for WCraft_HostTick (native\src\skycraft_host.hpp TickFlags)
// 1 = in game, 2 = menu open, 4 = loading, 256 = don't follow, 512 = force teleport

@addField( CCommonMapManager )
public var wcraftMgr : CWCraftManager;

@wrapMethod( CCommonMapManager ) function OnGameStarted()
{
	wrappedMethod();
	if ( !wcraftMgr )
	{
		wcraftMgr = new CWCraftManager in this;
	}
	wcraftMgr.Start();
}

statemachine class CWCraftManager
{
	public var showDebug : bool; // the status line every 10 s on screen: wcdebug(true)
	default showDebug = false;

	private var offX : array< int >;
	private var offY : array< int >;
	private var scanIndex : int;
	private var groups : array< name >;
	private var worldName : string;
	private var startTime : float;
	private var lastStatusTime : float;
	private var lastWorldCheck : float;
	private var selfTestDone : bool;
	private var ticks : int;
	private var blocks : CWCraftBlocks;
	private var ctl : CWCraftControl;
	private var controlLoop : CWCraftControlLoop;
	private var combat : CWCraftCombat;
	private var lights : CWCraftLights;     // Minecraft's torches etc. light the Witcher (witchercraft_lights.ws)

	public function GetBlocks() : CWCraftBlocks
	{
		return blocks;
	}

	public function GetControl() : CWCraftControl
	{
		return ctl;
	}

	public function GetCombat() : CWCraftCombat
	{
		return combat;
	}

	public function Start()
	{
		var i : int;
		var r : int;

		if ( offX.Size() == 0 )
		{
			// Square rings around the player, nearest first: r = 0..16 -> 33 x 33 samples.
			offX.PushBack( 0 );
			offY.PushBack( 0 );
			for ( r = 1; r <= 16; r += 1 )
			{
				for ( i = -r; i <= r; i += 1 )
				{
					offX.PushBack( i ); offY.PushBack( -r );
					offX.PushBack( i ); offY.PushBack( r );
				}
				for ( i = -r + 1; i <= r - 1; i += 1 )
				{
					offX.PushBack( -r ); offY.PushBack( i );
					offX.PushBack( r ); offY.PushBack( i );
				}
			}
			groups.PushBack( 'Terrain' );
			groups.PushBack( 'Static' );
			groups.PushBack( 'Destructible' );
			groups.PushBack( 'Platforms' );
			groups.PushBack( 'Boat' );
			groups.PushBack( 'Door' );
		}
		scanIndex = 0;
		worldName = "";
		selfTestDone = false;
		startTime = theGame.GetEngineTimeAsSeconds();
		lastStatusTime = startTime;
		lastWorldCheck = startTime - 10;
		WCraft_ControlNote( "manager start (" + offX.Size() + " sample offsets)" );
		if ( !blocks )
		{
			blocks = new CWCraftBlocks in this;
		}
		blocks.Init();
		if ( !ctl )
		{
			ctl = new CWCraftControl in this;
		}
		ctl.Init();
		if ( !controlLoop )
		{
			controlLoop = new CWCraftControlLoop in this;
		}
		controlLoop.Start( ctl );
		// M4a combat (witchercraft_combat.ws)
		if ( !combat )
		{
			combat = new CWCraftCombat in this;
		}
		combat.Init();

		// (Re)start the latent loop.
		if ( GetCurrentStateName() == 'WCraftRunning' )
		{
			GotoState( 'WCraftIdle' );
		}
		GotoState( 'WCraftRunning' );
	}

	public function Step()
	{
		var pos : Vector;
		var dir : Vector;
		var horiz : float;
		var pitch : float;
		var flags : int;
		var now : float;
		var path : string;
		var centre : Vector;

		if ( !thePlayer )
		{
			return;
		}
		now = theGame.GetEngineTimeAsSeconds();

		if ( now - lastWorldCheck > 2.0 )
		{
			lastWorldCheck = now;
			path = theGame.GetWorld().GetDepotPath();
			if ( path != worldName )
			{
				worldName = path;
				WCraft_SetWorld( path );
				blocks.OnWorldChanged();
				if ( lights )
				{
					lights.Clear();
				}
				WCraft_ControlNote( "world " + path );
			}
		}

		pos = thePlayer.GetWorldPosition();
		dir = theCamera.GetCameraDirection();
		horiz = SqrtF( dir.X * dir.X + dir.Y * dir.Y );
		pitch = Rad2Deg( AtanF( dir.Z, horiz ) ); // up = positive (native flips it for MC)

		flags = 1;
		if ( theGame.IsLoadingScreenVideoPlaying() )
		{
			flags = flags + 4;
		}
		if ( ctl && ctl.ConsumeTeleportRequest() )
		{
			flags = flags + 512; // after a scene / ride: Minecraft goes where Geralt is now
		}
		WCraft_HostTick( pos, thePlayer.GetHeading(), pitch, flags );
		// Ground is sampled around Minecraft's player while it drives (or a teleport target).
		centre = WCraft_SampleCentre();
		if ( centre.W > 0.5 )
		{
			centre.W = 1.0;
			SampleGround( centre );
		}
		else
		{
			SampleGround( pos );
		}
		// M5a: W3's real collision (walls, buildings, rocks, trees) around Minecraft's player, read
		// natively from PhysX within a per-call time budget (ini CollisionBudgetUs).
		WCraft_ColScan();
		blocks.Step();
		combat.Step( ctl && ctl.IsEngaged() );
		if ( !lights )
		{
			lights = new CWCraftLights in this;
		}
		lights.Step();
		ticks += 1;

		if ( !selfTestDone && now - startTime > 5.0 )
		{
			// The natives selftest (witchercraft_natives.ws), once the loading screen is gone: logged,
			// and shown on screen only if it failed (wcraft() shows it on demand).
			selfTestDone = true;
			if ( !WCraft_SelfTest( false ) )
			{
				theGame.GetGuiManager().ShowNotification( "WitcherCraft: the native bridge self-test failed (see witchercraft_native.log)" );
			}
			if ( ctl )
			{
				ctl.ClearOldSafetyNet();
			}
			lastStatusTime = now;
		}
		else if ( showDebug && now - lastStatusTime > 10.0 )
		{
			lastStatusTime = now;
			theGame.GetGuiManager().ShowNotification( "WitcherCraft: " + WCraft_GetMcStatus(), 4.0 );
		}
	}

	// Ground under up to 96 lattice points (integer W3 x/y) per tick, cycling through the
	// 33x33 ring list around the sampling centre (M6b: led ahead of a fast Minecraft player by
	// up to 12 m along its velocity, see WCraft_SampleCentre). Traced from 2.5 m above the feet so
	// roofs/branches overhead don't count; if nothing is below (a hillside rising above the
	// feet), traced again from 40 m up.
	private function SampleGround( pos : Vector )
	{
		var i : int;
		var n : int;
		var cx : int;
		var cy : int;
		var x : int;
		var y : int;
		var hit : Vector;
		var normal : Vector;
		var data : string;
		var world : CWorld;
		var a : Vector;
		var b : Vector;
		var wl : float;

		world = theGame.GetWorld();
		n = offX.Size();
		if ( n == 0 )
		{
			return;
		}
		cx = FloorF( pos.X );
		cy = FloorF( pos.Y );
		data = "";
		for ( i = 0; i < 96; i += 1 )
		{
			if ( scanIndex >= n )
			{
				scanIndex = 0;
			}
			x = cx + offX[ scanIndex ];
			y = cy + offY[ scanIndex ];
			scanIndex += 1;

			a = Vector( (float)x, (float)y, pos.Z + 2.5 );
			// M6b: 250 m down (was 30): flying high, a 30 m ray stored "no ground" under the
			// player, and diving onto that column found no floor in Minecraft.
			b = Vector( (float)x, (float)y, pos.Z - 250.0 );
			if ( world.StaticTrace( a, b, hit, normal, groups ) )
			{
				// M7: W3's water over that ground (lakes, rivers, the sea): "x,y,h,w" -> Minecraft
				// swims there (native WaterGrid). Same test as Geralt's grounding uses.
				wl = world.GetWaterLevel( hit );
				if ( wl > hit.Z + 0.1 && wl < hit.Z + 500.0 )
				{
					data += IntToString( x ) + "," + IntToString( y ) + "," + FloatToStringPrec( hit.Z, 2 ) + "," + FloatToStringPrec( wl, 2 ) + ";";
				}
				else
				{
					data += IntToString( x ) + "," + IntToString( y ) + "," + FloatToStringPrec( hit.Z, 2 ) + ";";
				}
			}
			else
			{
				a = Vector( (float)x, (float)y, pos.Z + 40.0 );
				b = Vector( (float)x, (float)y, pos.Z + 2.5 );
				if ( world.StaticTrace( a, b, hit, normal, groups ) )
				{
					data += IntToString( x ) + "," + IntToString( y ) + "," + FloatToStringPrec( hit.Z, 2 ) + ";";
				}
				else
				{
					// M7: deep water (the sea) with no bottom within reach: swim there, with a
					// floor 40 m under the surface instead of the void.
					a = Vector( (float)x, (float)y, pos.Z );
					wl = world.GetWaterLevel( a );
					if ( wl > pos.Z - 250.0 && wl < pos.Z + 40.0 )
					{
						data += IntToString( x ) + "," + IntToString( y ) + "," + FloatToStringPrec( wl - 40.0, 2 ) + "," + FloatToStringPrec( wl, 2 ) + ";";
					}
					else
					{
						data += IntToString( x ) + "," + IntToString( y ) + ",n;";
					}
				}
			}
		}
		WCraft_PushHeights( data );
	}
}

state WCraftIdle in CWCraftManager
{
}

state WCraftRunning in CWCraftManager
{
	event OnEnterState( prevStateName : name )
	{
		Run();
	}

	entry function Run()
	{
		while ( true )
		{
			parent.Step();
			Sleep( 0.033 );
		}
	}
}

function WCraft_Manager() : CWCraftManager
{
	return theGame.GetCommonMapManager().wcraftMgr;
}

exec function wcstatus()
{
	var fwd : Vector;
	var mc : Vector;
	var msg : string;

	// Heading convention check: W3 forward = (-sin h, cos h), so heading 90 must be (-1, 0).
	fwd = VecFromHeading( 90.0 );
	mc = WCraft_GetMcPos();
	msg = WCraft_GetMcStatus();
	msg += "<br>Geralt " + VecToStringPrec( thePlayer.GetWorldPosition(), 2 ) + " hdg " + FloatToStringPrec( thePlayer.GetHeading(), 1 );
	msg += "<br>MC (W3 coords) " + VecToStringPrec( mc, 2 );
	msg += "<br>VecFromHeading(90) = " + FloatToStringPrec( fwd.X, 2 ) + ", " + FloatToStringPrec( fwd.Y, 2 ) + " (expect -1, 0)";
	if ( !WCraft_Manager() )
	{
		msg += "<br>manager NOT running";
	}
	LogChannel( 'WitcherCraft', msg );
	theGame.GetGuiManager().ShowNotification( msg, 8.0 );
}

// M5a: W3 wall/building collision status; wccolon(false) switches the native source off.
exec function wccol()
{
	var msg : string;

	msg = WCraft_ColStatus();
	LogChannel( 'WitcherCraft', msg );
	theGame.GetGuiManager().ShowNotification( msg, 10.0 );
}

exec function wccolon( on : bool )
{
	var msg : string;

	WCraft_ColEnable( on );
	msg = WCraft_ColStatus();
	theGame.GetGuiManager().ShowNotification( msg, 10.0 );
}

exec function wcdebug( on : bool )
{
	var mgr : CWCraftManager;

	mgr = WCraft_Manager();
	if ( mgr )
	{
		mgr.showDebug = on;
	}
}
