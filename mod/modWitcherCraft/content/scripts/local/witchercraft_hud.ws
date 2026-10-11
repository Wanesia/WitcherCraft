// WitcherCraft M6a: The Witcher's HUD in Minecraft mode.
//
// Minecraft mode has Minecraft's identity: Minecraft's own HUD (hotbar, hearts, crosshair) is the
// HUD. The Witcher's HUD modules that describe Geralt (health / medallion, buffs, quick slots,
// minimap, quest tracker, key hints, companion, damaged items, oxygen) are switched off with the
// vanilla module API (CR4HudModuleBase.SetEnabled, as the HUD options menu and the console's
// HudSetModuleEnabled do). Everything that is about the WORLD stays: the interaction prompt
// (G does it), dialogue choices, subtitles, NPC one-liners, enemy names and
// health, boss bars, loot feed, journal / quest updates, area names, notifications, messages.
//
// Leaving Minecraft mode (F8, a scene, a dialogue, a ride, death) restores the player's own HUD
// settings with the vanilla CR4ScriptedHud.UpdateHudConfigs() (what the options menu applies).
// While Minecraft drives, the hide is re-applied every second (vanilla code re-enables some
// modules by itself, e.g. after the radial menu or focus combat).
//
// Enemy name and health bar: the game shows them for Geralt's display target, which it picks from
// hidden Geralt's heading and its own camera (r4Player.ws UpdateDisplayTarget), not from where
// Steve looks, so as Steve they came and went at random. While Minecraft drives (and the "Witcher
// enemy names" option is on), the target is the NPC nearest Steve's look ray instead: Focus()
// below and the CR4Player.UpdateDisplayTarget wrapper at the end of this file.
//
// Console: wchud( 0|1 )  (1 = hide The Witcher's HUD in Minecraft mode, default; 0 = keep it)

// Minecraft's WitcherCraft options (native\src\natives_control.cpp).
import function WCraft_Options() : int;

class CWCraftHud
{
	private var hiding : bool;        // our modules are switched off right now
	private var focusOff : bool;      // EnemyFocusModule was switched off by us (not by the game)
	private var wanted : bool;        // wchud(): hide in Minecraft mode at all
	private var wantedKnown : bool;
	private var lastApply : float;
	private var names : array< string >;

	// What to hide follows Minecraft's WitcherCraft options (its Esc menu; WCraft_Options bits:
	// 4 minimap, 8 quest tracker, 16 Geralt's HUD, 32 Witcher enemy names). 0 = an older Minecraft:
	// minimap and quest tracker shown, Geralt's HUD hidden, enemy names shown.
	private var opts : int;
	default opts = -1;
	// Enemy name / health bar as Steve (see the top of this file)
	private var focusOn : bool;           // Geralt's display target is ours right now
	private var focusTarget : CNewNPC;
	private var focusSeen : float;        // when Steve last looked at focusTarget
	private var lastFocusPick : float;
	private var focusLogs : int;
	private var focusGroups : array< name >;

	private function Names()
	{
		var o : int;

		o = opts;
		if ( o <= 0 )
		{
			o = 1 + 4 + 8 + 32;
		}
		names.Clear();
		names.PushBack( "ControlsFeedbackModule" );  // The Witcher's key hints (its keys don't apply to Steve)
		names.PushBack( "OxygenBarModule" );         // Minecraft shows Steve's own air
		if ( ( o & 16 ) == 0 )
		{
			names.PushBack( "WolfHeadModule" );      // Geralt's health / stamina / toxicity / medallion
			names.PushBack( "BuffsModule" );
			names.PushBack( "ItemInfoModule" );      // quick slots / selected bomb, crossbow, potions
			names.PushBack( "CompanionModule" );
			names.PushBack( "DamagedItemsModule" );
		}
		if ( ( o & 4 ) == 0 )
		{
			names.PushBack( "Minimap2Module" );
		}
		if ( ( o & 8 ) == 0 )
		{
			names.PushBack( "QuestsModule" );        // quest tracker
		}
		if ( ( o & 32 ) == 0 )
		{
			names.PushBack( "EnemyFocusModule" );    // enemy name + health bar
		}
		if ( ( o & 32 ) == 0 || ( o & 256 ) != 0 )
		{
			names.PushBack( "BossFocusModule" );     // off, or Minecraft's boss bar shows bosses instead
		}
	}

	// Every frame: picks up a change of Minecraft's options (the hide is redone from scratch).
	private function PollOptions()
	{
		var o : int;

		o = WCraft_Options();
		if ( o != opts )
		{
			opts = o;
			if ( hiding )
			{
				Restore( "options changed" );
			}
		}
	}

	public function IsWanted() : bool
	{
		if ( !wantedKnown )
		{
			wantedKnown = true;
			wanted = true;
		}
		return wanted;
	}

	public function SetWanted( on : bool )
	{
		wantedKnown = true;
		wanted = on;
		if ( !on && hiding )
		{
			Restore( "wchud(0)" );
		}
	}

	// Every frame from CWCraftControl.Frame(). mcDrives = Minecraft mode engaged (no scene etc.).
	public function Frame( mcDrives : bool )
	{
		var now : float;

		PollOptions();
		Focus( mcDrives );
		if ( mcDrives && IsWanted() )
		{
			now = theGame.GetEngineTimeAsSeconds();
			if ( !hiding || now - lastApply > 1.0 || now < lastApply )
			{
				lastApply = now;
				Hide();
			}
		}
		else if ( hiding )
		{
			Restore( "Minecraft mode off" );
		}
	}

	private function Hide()
	{
		var hud : CR4ScriptedHud;
		var module : CR4HudModuleBase;
		var i : int;
		var n : int;

		hud = (CR4ScriptedHud)theGame.GetHud();
		if ( !hud )
		{
			return;
		}
		Names();
		n = 0;
		for ( i = 0; i < names.Size(); i += 1 )
		{
			module = (CR4HudModuleBase)hud.GetHudModule( names[ i ] );
			if ( module && module.GetEnabled() )
			{
				module.SetEnabled( false );
				n += 1;
				if ( names[ i ] == "EnemyFocusModule" )
				{
					focusOff = true;
				}
			}
		}
		if ( !hiding )
		{
			hiding = true;
			WCraft_ControlNote( "hud: The Witcher's HUD hidden for Minecraft mode (" + n + " modules off; interaction prompt, dialogue and notifications kept)" );
		}
	}

	public function Restore( why : string )
	{
		var hud : CR4ScriptedHud;
		var module : CR4HudModuleBase;

		hiding = false;
		hud = (CR4ScriptedHud)theGame.GetHud();
		if ( !hud )
		{
			return;
		}
		// The player's own HUD options, exactly as the options menu applies them.
		hud.UpdateHudConfigs();
		// Except the enemy name / health bar: the game never switches that module off (its options
		// only set what it shows), so UpdateHudConfigs never switches it back on. After "Witcher enemy
		// names" was off once, it stayed off for the rest of the session. Only switched back on if we
		// switched it off, so a scene or quest that hides it keeps it hidden.
		module = (CR4HudModuleBase)hud.GetHudModule( "EnemyFocusModule" );
		if ( focusOff && module && !module.GetEnabled() )
		{
			module.SetEnabled( true );
		}
		focusOff = false;
		WCraft_ControlNote( "hud: The Witcher's HUD restored (" + why + ")" );
	}

	public function Status() : string
	{
		return "hud: hide in Minecraft mode " + IsWanted() + ", hidden now " + hiding + ", enemy name follows Steve's look " + focusOn;
	}

	// The CR4Player.UpdateDisplayTarget wrapper asks this: true = Geralt's display target is e.
	public function FocusTarget( out e : CGameplayEntity ) : bool
	{
		if ( !focusOn )
		{
			return false;
		}
		e = focusTarget;
		return true;
	}

	private function Focus( mcDrives : bool )
	{
		var o : int;
		var now : float;
		var pick : CNewNPC;

		o = opts;
		if ( o <= 0 )
		{
			o = 1 + 4 + 8 + 32;
		}
		if ( !mcDrives || ( o & 32 ) == 0 )
		{
			// Witcher mode (or names off): the game picks Geralt's target again by itself.
			focusOn = false;
			focusTarget = NULL;
			return;
		}
		now = theGame.GetEngineTimeAsSeconds();
		if ( focusOn && now - lastFocusPick < 0.1 && now >= lastFocusPick )
		{
			return;
		}
		lastFocusPick = now;
		pick = LookedAtNpc();
		if ( pick )
		{
			if ( pick != focusTarget && focusLogs < 20 )
			{
				focusLogs += 1;
				WCraft_ControlNote( "hud: enemy name follows Steve's look: " + pick.GetDisplayName() );
			}
			focusTarget = pick;
			focusSeen = now;
		}
		else if ( focusTarget && ( now - focusSeen > 1.5 || now < focusSeen ) )
		{
			// Kept for a moment after Steve looks away (no flicker at the edge of an enemy).
			focusTarget = NULL;
		}
		focusOn = true;
		thePlayer.WCraft_SetDisplayTarget( focusTarget );
	}

	// The living NPC with a name tooltip nearest Steve's look ray, within 30 m and not behind
	// the Witcher's ground or walls.
	private function LookedAtNpc() : CNewNPC
	{
		var body : Vector;
		var eye : Vector;
		var fwd : Vector;
		var aim : Vector;
		var hit : Vector;
		var normal : Vector;
		var ents : array< CGameplayEntity >;
		var npc : CNewNPC;
		var best : CNewNPC;
		var mac : CMovingPhysicalAgentComponent;
		var i : int;
		var h : float;
		var pr : float;
		var dx : float;
		var dy : float;
		var dz : float;
		var along : float;
		var px : float;
		var py : float;
		var pz : float;
		var perp : float;
		var reach : float;
		var score : float;
		var bestScore : float;

		body = WCraft_ControlBody();
		if ( body.W < 0.5 )
		{
			return NULL;
		}
		if ( focusGroups.Size() == 0 )
		{
			focusGroups.PushBack( 'Terrain' );
			focusGroups.PushBack( 'Static' );
		}
		eye = thePlayer.GetWorldPosition();
		eye.Z += body.Z;
		h = Deg2Rad( body.X );
		pr = Deg2Rad( body.Y );
		fwd = Vector( -SinF( h ) * CosF( pr ), CosF( h ) * CosF( pr ), SinF( pr ) );
		FindGameplayEntitiesInRange( ents, thePlayer, 30.0, 64, , FLAG_OnlyAliveActors + FLAG_ExcludePlayer, , 'CNewNPC' );
		best = NULL;
		bestScore = 1000.0;
		for ( i = 0; i < ents.Size(); i += 1 )
		{
			npc = (CNewNPC)ents[ i ];
			if ( !npc || !npc.IsAlive() || !npc.IsUsingTooltip() || !npc.GetGameplayVisibility() )
			{
				continue;
			}
			aim = npc.GetWorldPosition();
			reach = 0.8;
			mac = (CMovingPhysicalAgentComponent)npc.GetMovingAgentComponent();
			if ( mac )
			{
				aim.Z += mac.GetCapsuleHeight() * 0.6;
				reach = mac.GetCapsuleRadius() + 0.5;
			}
			else
			{
				aim.Z += 1.2;
			}
			dx = aim.X - eye.X;
			dy = aim.Y - eye.Y;
			dz = aim.Z - eye.Z;
			along = dx * fwd.X + dy * fwd.Y + dz * fwd.Z;
			if ( along < 0.5 )
			{
				continue;
			}
			px = dx - fwd.X * along;
			py = dy - fwd.Y * along;
			pz = dz - fwd.Z * along;
			perp = SqrtF( px * px + py * py + pz * pz );
			if ( perp > reach + along * 0.05 )
			{
				continue;
			}
			score = perp / along;
			if ( score >= bestScore )
			{
				continue;
			}
			if ( theGame.GetWorld().StaticTrace( eye, aim, hit, normal, focusGroups ) && VecDistance( eye, hit ) < along - 1.0 )
			{
				continue;
			}
			best = npc;
			bestScore = score;
		}
		return best;
	}
}

function WCraft_FocusTarget( out e : CGameplayEntity ) : bool
{
	var c : CWCraftControl;

	c = WCraft_Control();
	if ( !c )
	{
		return false;
	}
	return c.HudFocusTarget( e );
}

// SetDisplayTarget is protected: this is how the HUD script sets Geralt's display target.
@addMethod( CR4Player ) function WCraft_SetDisplayTarget( e : CGameplayEntity )
{
	SetDisplayTarget( e );
}

// While Steve's look picks the target (CWCraftHud.Focus), the game's own pick doesn't replace it.
@wrapMethod( CR4Player ) function UpdateDisplayTarget( optional forceUpdate : bool, optional forceNullActor : bool )
{
	var e : CGameplayEntity;

	if ( WCraft_FocusTarget( e ) )
	{
		SetDisplayTarget( e );
		return;
	}
	wrappedMethod( forceUpdate, forceNullActor );
}

exec function wchud( on : int )
{
	var c : CWCraftControl;

	c = WCraft_Control();
	if ( c )
	{
		c.SetHudHide( on != 0 );
		theGame.GetGuiManager().ShowNotification( "WitcherCraft: The Witcher's HUD in Minecraft mode: " + ( on == 0 ), 3.0 );
	}
}
