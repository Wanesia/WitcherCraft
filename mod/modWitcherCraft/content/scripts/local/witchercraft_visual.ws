// WitcherCraft M5b: Minecraft's world hidden behind Witcher geometry.
// Console:
//   wcocc()             status (also in witchercraft_native.log every 10 s)
//   wcoccon( 0|1 )      occlusion off / on                     (ini OverlayOcclusion)
//   wcoccbias( m )      how far in front a W3 surface must be  (ini OcclusionBias, default 0.15)
//   wcoccsoft( m )      soft edge width                        (ini OcclusionSoft, default 0.10)
//   wcocccand( n )      W3 depth candidate from the log, -1 = automatic (ini OcclusionDepthPick)
//   wcoccnext()         try the next depth candidate
//   wcoccnear( m )      W3 near plane until calibrated         (ini OcclusionNear)
//   wcoccfar( m )       W3 far plane, 0 = infinite              (ini OcclusionFar)
//   wcoccrev( -1|0|1 )  W3 reversed Z: auto / no / yes         (ini OcclusionReverseZ)
//   wcocccal()          calibrate: look ~45 deg down at flat open ground, standing (not crouched),
//                       or aim at a Minecraft block sitting on its W3 box; repeat 2-3 times
//   wcoccreset()        forget the calibration
//   wcoccdbg( 0|1|2 )   debug view: 1 = hidden parts red, 2 = show the W3 depth buffer in use
// Hotkeys (native): Ctrl+Shift+O on/off, Ctrl+Shift+C calibrate, Ctrl+Shift+D debug view, Ctrl+Shift+N next candidate.

import function WCraft_Occlusion( cmd : int, value : float ) : string;

function WCraftOccShow( msg : string )
{
	LogChannel( 'WitcherCraft', msg );
	theGame.GetGuiManager().ShowNotification( "WitcherCraft " + msg, 8.0 );
}

exec function wcocc()
{
	WCraftOccShow( WCraft_Occlusion( 0, 0.0 ) );
}

exec function wcoccon( on : int )
{
	WCraftOccShow( WCraft_Occlusion( 1, (float)on ) );
}

exec function wcoccbias( metres : float )
{
	WCraftOccShow( WCraft_Occlusion( 2, metres ) );
}

exec function wcoccsoft( metres : float )
{
	WCraftOccShow( WCraft_Occlusion( 10, metres ) );
}

exec function wcocccand( n : int )
{
	WCraftOccShow( WCraft_Occlusion( 3, (float)n ) );
}

exec function wcoccnext()
{
	WCraftOccShow( WCraft_Occlusion( 11, 0.0 ) );
}

exec function wcoccnear( metres : float )
{
	WCraftOccShow( WCraft_Occlusion( 4, metres ) );
}

exec function wcoccfar( metres : float )
{
	WCraftOccShow( WCraft_Occlusion( 5, metres ) );
}

exec function wcoccrev( mode : int )
{
	WCraftOccShow( WCraft_Occlusion( 6, (float)mode ) );
}

exec function wcocccal()
{
	WCraftOccShow( WCraft_Occlusion( 7, 0.0 ) );
}

exec function wcoccreset()
{
	WCraftOccShow( WCraft_Occlusion( 8, 0.0 ) );
}

exec function wcoccdbg( mode : int )
{
	WCraftOccShow( WCraft_Occlusion( 9, (float)mode ) );
}
