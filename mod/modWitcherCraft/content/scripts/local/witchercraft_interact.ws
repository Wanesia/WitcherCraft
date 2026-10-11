// WitcherCraft M5c: the Witcher interact key in Minecraft mode.
//
// G (native InteractKey) never reaches Minecraft. The native GetRawInputData hook hands it to
// The Witcher AS ITS OWN interaction key (E), so whatever W3's interaction system has selected
// (the HUD prompt) runs exactly as in Witcher mode: talk, loot, containers, doors, notice boards,
// horse, herbs, everything. Minecraft mode leaves the interaction actions unblocked for that.
//
// Every press is also reported here (WCraft_InteractPoll). If W3 had NO active interaction (or
// the key couldn't be handed over), this script picks the interaction component nearest to
// Minecraft's look ray within ~3.5 m, turns hidden Geralt towards it and fires its OnInteraction
// event itself, the same event the engine would call.
//
// Console: wcinteract()  (status)

import function WCraft_InteractPoll() : Vector;
import function WCraft_ControlBody() : Vector;

class CWCraftInteract
{
	private var lastActive : CInteractionComponent;
	private var hinted : bool;
	private var presses : int;
	private var engineHandled : int;
	private var fallbacks : int;
	private var misses : int;
	private var lastWhat : string;

	// Every frame, from CWCraftControl.Frame(). engaged = Minecraft drives; free = no scene /
	// dialogue / vehicle / fade, no W3 menu or popup.
	public function Frame( ctl : CWCraftControl, engaged : bool, free : bool )
	{
		var poll : Vector;
		var active : CInteractionComponent;
		var n : int;
		var given : int;

		poll = WCraft_InteractPoll();
		n = (int)poll.X;
		given = (int)poll.Y;
		active = theGame.GetInteractionsManager().GetActiveInteraction();

		// The W3 HUD shows its own prompt (with its own key, E); say once which key it is here.
		if ( engaged && free && active && active != lastActive && !hinted )
		{
			hinted = true;
			theGame.GetGuiManager().ShowNotification( "WitcherCraft: press G for Witcher interactions (talk, loot, doors...).<br>The prompt shows the Witcher's own key.", 6.0 );
		}
		lastActive = active;

		if ( n <= 0 )
		{
			return;
		}
		presses += n;
		if ( !engaged || !free )
		{
			// A scene / menu / popup already has the player (often the one this press opened).
			return;
		}
		if ( active && given > 0 )
		{
			// W3 got its interaction key with an interaction selected: the engine runs it.
			engineHandled += 1;
			lastWhat = "engine: " + active.GetActionName() + " on " + active.GetEntity().GetReadableName();
			WCraft_ControlNote( "interact: " + lastWhat );
			return;
		}
		Fallback( ctl );
	}

	// The interaction nearest to Minecraft's look ray, fired by hand.
	private function Fallback( ctl : CWCraftControl )
	{
		var body : Vector;
		var feet : Vector;
		var eye : Vector;
		var fwd : Vector;
		var d : Vector;
		var p : Vector;
		var ents : array< CGameplayEntity >;
		var comps : array< CComponent >;
		var ic : CInteractionComponent;
		var best : CInteractionComponent;
		var bestPoint : Vector;
		var i : int;
		var j : int;
		var h : float;
		var pr : float;
		var dist : float;
		var along : float;
		var perp : float;
		var score : float;
		var bestScore : float;
		var act : string;

		body = WCraft_ControlBody();
		if ( body.W < 0.5 )
		{
			return;
		}
		feet = thePlayer.GetWorldPosition();
		eye = feet;
		eye.Z += body.Z;
		h = Deg2Rad( body.X );
		pr = Deg2Rad( body.Y );
		fwd = Vector( -SinF( h ) * CosF( pr ), CosF( h ) * CosF( pr ), SinF( pr ) );

		FindGameplayEntitiesInRange( ents, thePlayer, 4.5, 60 );
		bestScore = 1000000.0;
		for ( i = 0; i < ents.Size(); i += 1 )
		{
			if ( !ents[ i ] || ents[ i ] == thePlayer )
			{
				continue;
			}
			comps = ents[ i ].GetComponentsByClassName( 'CInteractionComponent' );
			for ( j = 0; j < comps.Size(); j += 1 )
			{
				ic = (CInteractionComponent)comps[ j ];
				if ( !ic || !ic.IsEnabled() )
				{
					continue;
				}
				act = ic.GetActionName();
				if ( act == "" || act == "Finish" )
				{
					continue;
				}
				if ( thePlayer.IsInCombat() && !ic.IsEnabledInCombat() )
				{
					continue;
				}
				p = ic.GetWorldPosition();
				if ( (CActor)ents[ i ] )
				{
					p = ents[ i ].GetWorldPosition();
					p.Z += 1.2;
				}
				d = p - eye;
				dist = VecLength( d );
				if ( dist > 3.5 )
				{
					continue;
				}
				along = VecDot( d, fwd );
				if ( along < 0.1 && dist > 1.0 )
				{
					continue;
				}
				perp = SqrtF( MaxF( 0.0, dist * dist - along * along ) );
				if ( perp > 1.0 + 0.3 * along )
				{
					continue;
				}
				score = perp + 0.2 * dist;
				if ( score < bestScore )
				{
					bestScore = score;
					best = ic;
					bestPoint = p;
				}
			}
		}
		if ( !best )
		{
			misses += 1;
			lastWhat = "nothing in front";
			WCraft_ControlNote( "interact: nothing to interact with in front (" + ents.Size() + " entities within 4.5 m)" );
			theGame.GetGuiManager().ShowNotification( "WitcherCraft: nothing to interact with here", 2.0 );
			return;
		}
		// Hidden Geralt faces the target first (some interactions check where he looks).
		d = bestPoint - feet;
		ctl.FaceHeading( VecHeading( d ), 1.5 );
		if ( Fire( best ) )
		{
			fallbacks += 1;
			lastWhat = "script: " + best.GetActionName() + " on " + best.GetEntity().GetReadableName();
		}
		else
		{
			misses += 1;
			lastWhat = "unsupported: " + best.GetActionName() + " on " + best.GetEntity().GetReadableName();
			theGame.GetGuiManager().ShowNotification( "WitcherCraft: can't do '" + best.GetActionName() + "' from Minecraft mode, press " + WCraft_KeyName( 1 ) + " and use E", 4.0 );
		}
		WCraft_ControlNote( "interact: " + lastWhat );
	}

	// The engine calls OnInteraction( actionName, activator ) on the entity (or the scripted
	// component) that owns the interaction. CGameplayEntity doesn't declare it in script, so
	// every vanilla class that does is cast to explicitly.
	private function Fire( ic : CInteractionComponent ) : bool
	{
		var ent : CEntity;
		var act : string;
		var npc : CNewNPC;
		var horse : W3HorseComponent;

		ent = ic.GetEntity();
		act = ic.GetActionName();
		npc = (CNewNPC)ent;
		if ( npc )
		{
			horse = npc.GetHorseComponent();
			if ( horse && act != "Talk" )
			{
				horse.OnInteraction( act, thePlayer );
				return true;
			}
			npc.OnInteraction( act, thePlayer );
			return true;
		}
		if ( (CGameplayLightComponent)ic )
		{
			ic.OnInteraction( act, thePlayer );
			return true;
		}
		if ( (W3LockableEntity)ent )                // containers, loot bags, corpses, doors
		{
			((W3LockableEntity)ent).OnInteraction( act, thePlayer );
			return true;
		}
		if ( (W3NoticeBoard)ent )
		{
			((W3NoticeBoard)ent).OnInteraction( act, thePlayer );
			return true;
		}
		if ( (W3AnimationInteractionEntity)ent )    // beds, clues, traps...
		{
			((W3AnimationInteractionEntity)ent).OnInteraction( act, thePlayer );
			return true;
		}
		if ( (W3AlchemyTable)ent )
		{
			((W3AlchemyTable)ent).OnInteraction( act, thePlayer );
			return true;
		}
		if ( (W3SmartObject)ent )
		{
			((W3SmartObject)ent).OnInteraction( act, thePlayer );
			return true;
		}
		if ( (W3ItemRepairObject)ent )
		{
			((W3ItemRepairObject)ent).OnInteraction( act, thePlayer );
			return true;
		}
		if ( (W3Stash)ent )
		{
			((W3Stash)ent).OnInteraction( act, thePlayer );
			return true;
		}
		if ( (W3InteractionSwitch)ent )
		{
			((W3InteractionSwitch)ent).OnInteraction( act, thePlayer );
			return true;
		}
		if ( (CItemInteractionEntity)ent )
		{
			((CItemInteractionEntity)ent).OnInteraction( act, thePlayer );
			return true;
		}
		if ( (W3Poster)ent )
		{
			((W3Poster)ent).OnInteraction( act, thePlayer );
			return true;
		}
		if ( (CTeleportEntity)ent )
		{
			((CTeleportEntity)ent).OnInteraction( act, thePlayer );
			return true;
		}
		return false;
	}

	public function Status() : string
	{
		return "interact: presses " + presses + ", engine " + engineHandled + ", script " + fallbacks + ", missed " + misses + ", last: " + lastWhat;
	}
}

exec function wcinteract()
{
	var c : CWCraftControl;
	var msg : string;
	var active : CInteractionComponent;

	msg = "no control";
	c = WCraft_Control();
	if ( c )
	{
		msg = c.InteractStatus();
	}
	active = theGame.GetInteractionsManager().GetActiveInteraction();
	if ( active )
	{
		msg += "<br>W3 active interaction: " + active.GetActionName() + " on " + active.GetEntity().GetReadableName();
	}
	else
	{
		msg += "<br>W3 active interaction: none";
	}
	LogChannel( 'WitcherCraft', msg );
	WCraft_ControlNote( "wcinteract: " + msg );
	theGame.GetGuiManager().ShowNotification( msg, 6.0 );
}
