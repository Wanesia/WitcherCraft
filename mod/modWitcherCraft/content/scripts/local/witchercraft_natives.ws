// WitcherCraft: native bridge self-test.
// The natives below are registered at engine init by dinput8.dll (native\).
// If the DLL is missing, script compilation fails on these imports.

import function WCraft_Ping() : int;
import function WCraft_GetBlockCount() : int;
import function WCraft_SetPlayerPos( pos : Vector ) : bool;
import function WCraft_GetLastPos() : Vector;
import function WCraft_Echo( msg : string ) : string;
import function WCraft_Rpc( request : string ) : string;

function WCraft_SelfTest( optional verbose : bool ) : bool
{
	var ping : int;
	var echo : string;
	var back : Vector;
	var ok : bool;

	ping = WCraft_Ping();
	WCraft_SetPlayerPos( thePlayer.GetWorldPosition() );
	back = WCraft_GetLastPos();
	echo = WCraft_Echo( "ping=" + ping + " blocks=" + WCraft_GetBlockCount() );

	ok = ( ping == 1337 );
	WCraft_ControlNote( "selftest ok=" + ok + " echo=" + echo + " lastPos=" + VecToString( back ) );
	if ( verbose )
	{
		theGame.GetGuiManager().ShowNotification( "WitcherCraft native bridge: " + ok + "<br>" + echo + "<br>pos " + VecToString( back ) );
	}
	return ok;
}

exec function wcraft()
{
	WCraft_SelfTest( true );
}

// The selftest runs automatically ~5 s after the game has started (after the loading screen; its
// notification only shows if it failed): see CWCraftManager.Step in witchercraft.ws, which owns the
// single CCommonMapManager.OnGameStarted wrapper.
