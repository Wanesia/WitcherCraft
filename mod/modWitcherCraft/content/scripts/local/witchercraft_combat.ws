// WitcherCraft M4a: combat between Steve (Minecraft) and The Witcher's actors.
// SkyCraft's Combat.cpp semantics, done with vanilla W3 script APIs:
//   - every 0.05 s while Minecraft drives: W3 actors within CombatRange (default 32 m) ->
//     WCraft_CombatActors -> the shared ActorTable -> invisible hittable stand-ins in Minecraft
//   - Minecraft's hits on them come back (WCraft_CombatPop) and are applied as W3DamageActions from
//     the player through theGame.damageMgr (hit reactions, aggro, death, loot, kill credit)
//   - W3 hits on the hidden Geralt are cancelled (W3DamageManager.ProcessAction /
//     W3PlayerWitcher.ReduceDamage wrappers) and forwarded to Minecraft as kInHurt
//   - Steve died: CombatDeathMode=0 Minecraft respawns him where he fell (instantly, inventory kept),
//     1 Geralt dies too (SkyCraft parity: W3 game over)
// Actor ids are CEntity.GetGuidHash(). Console: wccombat()  wchurt( percent )

import function WCraft_CombatActors( data : string ) : int;
import function WCraft_CombatHurt( kind : int, damage : float, maxHealth : float, attacker : int, flags : int ) : float;
import function WCraft_CombatPop() : int;
import function WCraft_CombatEvInt( i : int ) : int;
import function WCraft_CombatEvVec() : Vector;
import function WCraft_CombatParam( key : string ) : float;
import function WCraft_CombatStatus() : string;

class CWCraftCombat
{
	private var enabled : bool;
	private var actorRange : float;
	private var hitType : int;
	private var deathMode : int;
	private var engaged : bool;
	private var published : bool;
	private var lastGather : float;
	private var ids : array< int >;
	private var actors : array< CActor >;
	// the player-hit hook (BeginPlayerHit -> ReduceDamage -> EndPlayerHit)
	private var inPlayerHit : bool;
	private var hpBefore : float;
	private var captured : float;
	private var killing : bool;
	private var hitsApplied : int;
	private var hitsMissed : int;
	private var hurtsSent : int;
	private var logs : int;
	// Actors that refused EET_Burning (fire-proof, friendly, underwater): no Minecraft fire on their
	// stand-ins for a while (actor flag 64), instead of a retry every second.
	private var noFireIds : array< int >;
	private var noFireUntil : array< float >;

	public function Init()
	{
		enabled = WCraft_CombatParam( "enabled" ) > 0.5;
		actorRange = WCraft_CombatParam( "range" );
		if ( actorRange < 4.0 )
		{
			actorRange = 32.0;
		}
		hitType = (int)WCraft_CombatParam( "hitType" );
		deathMode = (int)WCraft_CombatParam( "deathMode" );
		engaged = false;
		published = false;
		inPlayerHit = false;
		killing = false;
		lastGather = 0.0;
		ids.Clear();
		actors.Clear();
		WCraft_ControlNote( "combat: ready (enabled " + enabled + ", range " + FloatToStringPrec( actorRange, 0 ) + " m, hit type " + hitType + ", death mode " + deathMode + ")" );
	}

	// The manager's ~30 Hz step. isEngaged = Minecraft drives the player (CWCraftControl).
	public function Step( isEngaged : bool )
	{
		var now : float;

		if ( !enabled || !thePlayer )
		{
			return;
		}
		engaged = isEngaged && thePlayer.IsAlive();
		now = theGame.GetEngineTimeAsSeconds();
		if ( now - lastGather >= 0.05 || now < lastGather )
		{
			lastGather = now;
			if ( engaged )
			{
				PushActors();
			}
			else if ( published )
			{
				// Witcher mode: no stand-ins (SkyCraft clears the table when not puppeting).
				WCraft_CombatActors( "" );
				published = false;
				ids.Clear();
				actors.Clear();
			}
		}
		PollEvents();
	}

	// ---- W3 actors -> Minecraft ---------------------------------------------------------------
	private function PushActors()
	{
		var ents : array< CGameplayEntity >;
		var horse : CNewNPC;
		var actor : CActor;
		var mac : CMovingPhysicalAgentComponent;
		var pos : Vector;
		var i : int;
		var id : int;
		var flags : int;
		var w : float;
		var h : float;
		var hp : float;
		var nm : string;
		var data : string;

		FindGameplayEntitiesInRange( ents, thePlayer, actorRange, 64, , FLAG_OnlyAliveActors + FLAG_ExcludePlayer, , 'CActor' );
		horse = thePlayer.GetHorseWithInventory();
		ids.Clear();
		actors.Clear();
		data = "";
		for ( i = 0; i < ents.Size(); i += 1 )
		{
			actor = (CActor)ents[ i ];
			if ( !actor || ents[ i ] == thePlayer || ents[ i ] == horse || !actor.IsAlive() )
			{
				continue;
			}
			id = actor.GetGuidHash();
			if ( id == 0 || ids.Contains( id ) )
			{
				continue;
			}
			pos = actor.GetWorldPosition();
			w = 0.6;
			h = 1.8;
			mac = (CMovingPhysicalAgentComponent)actor.GetMovingAgentComponent();
			if ( mac )
			{
				w = mac.GetCapsuleRadius() * 2.0;
				h = mac.GetCapsuleHeight();
			}
			hp = actor.GetHealthPercents();
			flags = 0;
			if ( GetAttitudeBetween( thePlayer, actor ) == AIA_Hostile )
			{
				flags += 1;
			}
			if ( actor.IsImmortal() || actor.IsInvulnerable() )
			{
				flags += 4;
			}
			if ( actor.IsInCombat() )
			{
				flags += 8;
			}
			// A boss (the same test as the game's own, yrdenHealthDrain.ws): Minecraft shows a boss bar.
			if ( actor.HasAbility( 'SkillBoss' ) || actor.HasAbility( 'Boss' ) || actor.HasAbility( 'MonsterMHBoss' )
				|| actor.GetCharacterStats().HasAbilityWithTag( 'Boss' ) || (W3MonsterHuntNPC)actor
				|| ( actor.HasTag( 'HideHealthBarModule' ) && !actor.HasTag( 'NotBoss' ) ) )
			{
				flags += 16;
			}
			// Burning (Minecraft's water on its stand-in puts it out).
			if ( actor.HasBuff( EET_Burning ) )
			{
				flags += 32;
			}
			// The Witcher can't set it burning: an actor with no health (city crowd NPCs: no ability or
			// effect manager, GetHealthPercents() -1; Steve can't hurt them either) or one that just
			// refused EET_Burning. Minecraft then keeps its own fire off the stand-in.
			if ( hp < 0.0 || NoFire( id, theGame.GetEngineTimeAsSeconds() ) )
			{
				flags += 64;
			}
			nm =StrReplaceAll( actor.GetDisplayName(), ";", " " );
			ids.PushBack( id );
			actors.PushBack( actor );
			data += IntToString( id ) + "," + FloatToStringPrec( pos.X, 2 ) + "," + FloatToStringPrec( pos.Y, 2 ) + "," + FloatToStringPrec( pos.Z, 2 ) + ","
				+ FloatToStringPrec( actor.GetHeading(), 1 ) + "," + FloatToStringPrec( w, 2 ) + "," + FloatToStringPrec( h, 2 ) + ","
				+ FloatToStringPrec( hp, 3 ) + "," + IntToString( flags ) + "," + IntToString( actor.GetLevel() ) + "," + nm + ";";
		}
		WCraft_CombatActors( data );
		published = true;
	}

	private function FindActor( id : int ) : CActor
	{
		var i : int;

		for ( i = 0; i < ids.Size(); i += 1 )
		{
			if ( ids[ i ] == id )
			{
				return actors[ i ];
			}
		}
		return NULL;
	}

	// Did this actor refuse EET_Burning in the last 10 s? (Expired entries are dropped here.)
	private function NoFire( id : int, now : float ) : bool
	{
		var i : int;
		var found : bool;

		found = false;
		for ( i = noFireIds.Size() - 1; i >= 0; i -= 1 )
		{
			if ( noFireUntil[ i ] < now || noFireUntil[ i ] > now + 60.0 )
			{
				noFireIds.Erase( i );
				noFireUntil.Erase( i );
			}
			else if ( noFireIds[ i ] == id )
			{
				found = true;
			}
		}
		return found;
	}

	// ---- Minecraft -> W3 ------------------------------------------------------------------------
	private function PollEvents()
	{
		var type : int;
		var n : int;

		n = 0;
		while ( n < 32 )
		{
			type = WCraft_CombatPop();
			if ( type == 0 )
			{
				break;
			}
			n += 1;
			if ( type == 2 )
			{
				PlayerDied( WCraft_CombatEvInt( 0 ) );
			}
			else if ( !engaged )
			{
				// Stale hits from before Witcher mode: dropped, like SkyCraft when not puppeting.
				continue;
			}
			else if ( type == 1 )
			{
				ApplyHit( WCraft_CombatEvInt( 0 ), WCraft_CombatEvInt( 1 ), WCraft_CombatEvInt( 2 ), WCraft_CombatEvVec() );
			}
			else if ( type == 3 )
			{
				Explosion( WCraft_CombatEvVec() );
			}
			else if ( type == 7 )
			{
				ActorFire( WCraft_CombatEvInt( 0 ), WCraft_CombatEvVec() );
			}
		}
	}

	// Minecraft fire (lava, fire blocks, fire aspect, flame arrows, fire charges) on a stand-in sets
	// the actor burning the Witcher's way (flails, panics, takes fire damage; fire-proof monsters
	// refuse it). v.X = seconds; 0 = Minecraft water (a bucket, a splash water bottle) put it out.
	private function ActorFire( id : int, v : Vector )
	{
		var actor : CActor;
		var params : SCustomEffectParams;
		var res : EEffectInteract;

		actor = FindActor( id );
		if ( !actor || !actor.IsAlive() )
		{
			return;
		}
		if ( v.X <= 0.0 )
		{
			if ( actor.HasBuff( EET_Burning ) )
			{
				actor.RemoveAllBuffsOfType( EET_Burning );
				if ( logs < 50 )
				{
					logs += 1;
					WCraft_ControlNote( "combat: Minecraft water put out " + actor.GetDisplayName() );
				}
			}
			return;
		}
		if ( actor.HasBuff( EET_Burning ) )
		{
			return;
		}
		// No health type (city crowd NPCs): no effect manager either, AddEffectCustom would only assert.
		if ( actor.GetHealthPercents() < 0.0 )
		{
			RefuseFire( actor, id, "it has no health (a crowd NPC)" );
			return;
		}
		params.effectType = EET_Burning;
		params.creator = thePlayer;
		params.sourceName = "WitcherCraft";
		params.duration = ClampF( v.X, 1.0, 15.0 );
		res = actor.AddEffectCustom( params );
		if ( !EffectInteractionSuccessfull( res ) )
		{
			// The Witcher's own rules (effectManager.ws InternalAddEffect): underwater, immune to
			// burning (fire-proof monsters), or friendly to Geralt (no negative buffs from a friend).
			RefuseFire( actor, id, "the Witcher refused it (fire-proof, friendly or underwater)" );
			return;
		}
		if ( logs < 50 )
		{
			logs += 1;
			WCraft_ControlNote( "combat: Minecraft fire on " + actor.GetDisplayName() + " for " + FloatToStringPrec( params.duration, 0 ) + " s -> burning" );
		}
	}

	private function RefuseFire( actor : CActor, id : int, why : string )
	{
		if ( !noFireIds.Contains( id ) )
		{
			noFireIds.PushBack( id );
			noFireUntil.PushBack( theGame.GetEngineTimeAsSeconds() + 10.0 );
		}
		if ( logs < 50 )
		{
			logs += 1;
			WCraft_ControlNote( "combat: Minecraft fire on " + actor.GetDisplayName() + " (" + id + ") not applied: " + why );
		}
	}

	// v.X = fraction of max health, v.Y = Minecraft damage, v.Z = knockback strength, v.W = push heading.
	// flags: 1 critical, 2 projectile (arrow, crossbow bolt, thrown trident, ...), 4 sweep, 8 fire (lava,
	// fire blocks, fireballs). weapon: 0 fist / none, 1 blade, 2 axe, 3 blunt, 4 pierce (trident), 5 arrow.
	private function ApplyHit( id : int, flags : int, weapon : int, v : Vector )
	{
		var actor : CActor;
		var action : W3DamageAction;
		var maxHealth : float;
		var dmg : float;
		var hrt : EHitReactionType;
		var dmgName : name;
		var crit : bool;
		var fire : bool;
		var projectile : bool;
		var downed : bool;
		var byMob : bool;
		var note : string;

		actor = FindActor( id );
		if ( !actor || !actor.IsAlive() )
		{
			hitsMissed += 1;
			if ( logs < 50 )
			{
				logs += 1;
				WCraft_ControlNote( "combat: hit on unknown or dead actor " + id );
			}
			return;
		}
		// GetMaxHealth() is the vitality OR the essence maximum, and -1 for an actor with neither (city
		// crowd NPCs: vanilla can't hurt them, and a hit on one without an ability manager would even
		// call OnDeath, damageManagerProcessor.ws ProcessActionDamage).
		maxHealth = actor.GetMaxHealth();
		dmg = v.X * maxHealth;
		if ( maxHealth <= 0.0 || dmg <= 0.0 )
		{
			hitsMissed += 1;
			return;
		}
		crit = ( flags & 1 ) != 0;
		projectile = ( flags & 2 ) != 0;
		fire = ( flags & 8 ) != 0;
		byMob = ( flags & 16 ) != 0;
		// Sprint hits, knockback and crits stagger harder (SkyCraft: push > 0.45 or critical).
		hrt = EHRT_Light;
		if ( crit || v.Z > 0.45 )
		{
			hrt = EHRT_Heavy;
		}
		if ( hitType == 1 )
		{
			dmgName = theGame.params.DAMAGE_NAME_DIRECT;
		}
		else if ( fire )
		{
			// Lava, fire blocks, fireballs: the Witcher's fire damage (hits vitality and essence;
			// CDS_FireRes applies, so fire-proof monsters shrug it off as they do Igni).
			dmgName = theGame.params.DAMAGE_NAME_FIRE;
		}
		else if ( weapon == 4 || weapon == 5 )
		{
			dmgName = theGame.params.DAMAGE_NAME_PIERCING;
		}
		else if ( weapon == 1 || weapon == 2 )
		{
			dmgName = theGame.params.DAMAGE_NAME_SLASHING;
		}
		else
		{
			dmgName = theGame.params.DAMAGE_NAME_BLUDGEONING;
		}

		// A plain damage action from the player (like a bomb's): not flagged melee/ranged, so the
		// processor's sword-skill code (which expects a W3Action_Attack) stays out of it. Geralt
		// stands at Steve's feet, so the hit direction and the aggro target are right.
		// A Minecraft mob's hit (a zombie's swing, a golem's slam, a skeleton's arrow) has no attacker,
		// like the game's own damage areas (damageAreaEntity.ws): Geralt gets no aggro or kill credit.
		action = new W3DamageAction in theGame.damageMgr;
		if ( byMob )
		{
			action.Initialize( NULL, actor, NULL, "WitcherCraft", hrt, CPS_Undefined, false, false, false, false );
		}
		else
		{
			action.Initialize( thePlayer, actor, thePlayer, "WitcherCraft", hrt, CPS_Undefined, false, false, false, false );
		}
		action.AddDamage( dmgName, dmg );
		// Physical damage (slashing / piercing / bludgeoning) only lowers VITALITY: monsters live on
		// ESSENCE, which only silver (and elemental) damage reaches (damageManagerTypes.ws
		// DamageHitsEssence / DamageHitsVitality). Like the game's own arrows (arrowProjectile.ws
		// ProcessDamageAction: piercing + silver), add the same amount as silver: a human takes the
		// physical part, a monster the silver part (resisted by its CDS_PhysicalRes), never both.
		if ( hitType != 1 && !fire )
		{
			action.AddDamage( theGame.params.DAMAGE_NAME_SILVER, dmg );
		}
		action.SetPointResistIgnored( true );   // armour points ignored; % resistances still apply
		action.SetCannotReturnDamage( true );
		if ( crit )
		{
			action.SetCriticalHit();
		}
		// Arrows, crossbow bolts, thrown tridents: like Geralt's crossbow, a flying (or airborne)
		// monster hit by one comes down. Vanilla does this in CNewNPC.ReactToBeingHit (npc.ws) with
		// EET_Knockdown once a hit takes healthLossToForceLand_perc of its health; for Minecraft's
		// projectiles every hit counts. Knockdown-immune monsters stay immune.
		downed = false;
		if ( projectile && IsAirborne( actor ) )
		{
			action.AddEffectInfo( EET_Knockdown );
			action.SetProcessBuffsIfNoDamage( true );
			downed = true;
		}
		theGame.damageMgr.ProcessAction( action );
		delete action;
		hitsApplied += 1;
		if ( logs < 50 )
		{
			logs += 1;
			note = "";
			if ( downed )
			{
				note = "; shot down (knockdown)";
			}
			if ( byMob )
			{
				note += "; by a Minecraft mob";
			}
			WCraft_ControlNote( "combat: Minecraft hit " + actor.GetDisplayName() + " (" + id + ") for " + FloatToStringPrec( v.Y, 1 ) + " -> "
				+ FloatToStringPrec( dmg, 0 ) + " " + dmgName + " of " + FloatToStringPrec( maxHealth, 0 ) + "; health now " + FloatToStringPrec( actor.GetHealthPercents() * 100.0, 0 ) + " %" + note );
		}
	}

	// The test the game uses to force a flying monster down (CNewNPC.ReactToBeingHit, npc.ws): flying
	// stance, or off the ground and not swimming or riding.
	private function IsAirborne( actor : CActor ) : bool
	{
		var npc : CNewNPC;
		var mpac : CMovingPhysicalAgentComponent;

		npc = (CNewNPC)actor;
		if ( !npc )
		{
			return false;
		}
		if ( npc.GetCurrentStance() == NS_Fly )
		{
			return true;
		}
		if ( npc.IsUsingVehicle() || npc.GetCurrentStance() == NS_Swim )
		{
			return false;
		}
		mpac = (CMovingPhysicalAgentComponent)npc.GetMovingAgentComponent();
		if ( !mpac )
		{
			return false;
		}
		return !mpac.IsOnGround();
	}

	// TNT and creepers: the damage itself already arrived as hits on the stand-ins; this is the shove.
	private function Explosion( v : Vector )
	{
		var centre : Vector;
		var i : int;
		var d : float;

		centre = Vector( v.X, v.Y, v.Z );
		for ( i = 0; i < actors.Size(); i += 1 )
		{
			if ( !actors[ i ] || !actors[ i ].IsAlive() )
			{
				continue;
			}
			d = VecDistance( actors[ i ].GetWorldPosition(), centre );
			if ( d < v.W )
			{
				actors[ i ].AddEffectDefault( EET_Knockdown, thePlayer, "WitcherCraft" );
			}
			else if ( d < v.W * 2.0 )
			{
				actors[ i ].AddEffectDefault( EET_Stagger, thePlayer, "WitcherCraft" );
			}
		}
	}

	private function PlayerDied( killer : int )
	{
		var attacker : CActor;

		if ( deathMode == 1 && thePlayer.IsAlive() )
		{
			// SkyCraft parity: dying in Minecraft is dying in The Witcher.
			WCraft_ControlNote( "combat: Steve died; killing Geralt (CombatDeathMode=1)" );
			attacker = FindActor( killer );
			killing = true;
			thePlayer.Kill( 'WitcherCraft', true, attacker );
			killing = false;
			return;
		}
		// SkyCraft's world has immediate_respawn and keepInventory on: Steve is back at once, with his
		// things, at the host position (Geralt, who stands where Steve fell).
		WCraft_ControlNote( "combat: Steve died (killer " + killer + "); Minecraft respawned him here" );
		theGame.GetGuiManager().ShowNotification( "WitcherCraft: you died in Minecraft and respawned where you fell.", 4.0 );
	}

	// ---- W3 -> Minecraft: the player-hit hook --------------------------------------------------
	public function TakesPlayerHit( act : W3DamageAction ) : bool
	{
		return enabled && engaged && !killing && !inPlayerHit && act.victim == thePlayer && thePlayer.IsAlive();
	}

	// Before W3 processes a hit on the hidden Geralt: nothing of it may reach his body.
	public function BeginPlayerHit( act : W3DamageAction )
	{
		var dts : array< SRawDamage >;
		var i : int;
		var direct : float;

		inPlayerHit = true;
		captured = 0.0;
		hpBefore = thePlayer.GetStat( BCS_Vitality );
		// Direct damage is added after ReduceDamage, so take it out here and forward it ourselves.
		direct = act.GetDamageValue( theGame.params.DAMAGE_NAME_DIRECT );
		if ( direct > 0.0 && !act.GetIgnoreImmortalityMode() )
		{
			act.GetDTs( dts );
			act.ClearDamage();
			for ( i = 0; i < dts.Size(); i += 1 )
			{
				if ( dts[ i ].dmgType != theGame.params.DAMAGE_NAME_DIRECT )
				{
					act.AddDamage( dts[ i ].dmgType, dts[ i ].dmgVal );
				}
			}
			captured += direct;
		}
		// No knockdowns, bleeding or poison on a body Minecraft is driving; no hit animation.
		act.ClearEffects();
		act.SetHitAnimationPlayType( EAHA_ForceNo );
		act.SetCanPlayHitParticle( false );
	}

	// Called from W3PlayerWitcher.ReduceDamage: what W3 computed (after Geralt's armour) goes to
	// Minecraft instead. True = handled (skip W3's own dodge / Quen / parry reduction).
	public function CaptureReduceDamage( damageData : W3DamageAction ) : bool
	{
		if ( !inPlayerHit )
		{
			return false;
		}
		captured += damageData.processedDmg.vitalityDamage;
		damageData.SetAllProcessedDamageAs( 0 );
		return true;
	}

	public function EndPlayerHit( act : W3DamageAction )
	{
		var hpNow : float;
		var kind : int;
		var flags : int;
		var attacker : CActor;
		var attackerId : int;
		var mc : float;

		inPlayerHit = false;
		hpNow = thePlayer.GetStat( BCS_Vitality );
		if ( hpNow < hpBefore )
		{
			// Anything that still got through (scripted drains): Minecraft owns the health.
			captured += hpBefore - hpNow;
			thePlayer.ForceSetStat( BCS_Vitality, hpBefore );
		}
		if ( captured <= 0.0 )
		{
			return;
		}
		kind = 3;
		if ( act.IsActionMelee() )
		{
			kind = 0;
		}
		else if ( act.IsActionRanged() )
		{
			kind = 1;
		}
		else if ( act.IsActionWitcherSign() || act.IsDoTDamage() )
		{
			kind = 2;
		}
		flags = 0;
		if ( act.GetHitReactionType() == EHRT_Heavy )
		{
			flags += 2;
		}
		attackerId = 0;
		attacker = (CActor)act.attacker;
		if ( attacker )
		{
			attackerId = attacker.GetGuidHash();
		}
		mc = WCraft_CombatHurt( kind, captured, thePlayer.GetStatMax( BCS_Vitality ), attackerId, flags );
		hurtsSent += 1;
		if ( logs < 50 )
		{
			logs += 1;
			WCraft_ControlNote( "combat: Geralt hit for " + FloatToStringPrec( captured, 1 ) + " (kind " + kind + ") -> " + FloatToStringPrec( mc, 2 ) + " Minecraft damage" );
		}
	}

	public function Status() : string
	{
		return WCraft_CombatStatus() + "<br>engaged " + engaged + ", tracked " + ids.Size() + ", hits applied " + hitsApplied + " (missed " + hitsMissed + "), hurts sent " + hurtsSent
			+ "<br>range " + FloatToStringPrec( actorRange, 0 ) + " m, hit type " + hitType + ", death mode " + deathMode;
	}
}

function WCraft_Combat() : CWCraftCombat
{
	var mgr : CWCraftManager;

	mgr = WCraft_Manager();
	if ( !mgr )
	{
		return NULL;
	}
	return mgr.GetCombat();
}

// Every damage action in the game passes here. Only hits on the player while Minecraft drives are
// touched (TakesPlayerHit).
@wrapMethod( W3DamageManager ) function ProcessAction( act : W3DamageAction )
{
	var c : CWCraftCombat;
	var ctl : CWCraftControl;
	var take : bool;

	// The 5.0 compiler allows only ONE wrappedMethod call per wrapper.
	c = NULL;
	take = false;
	// Fall damage just after Minecraft mode gave Geralt back (actor.ws ApplyFallingDamage: no
	// attacker, source "FallingDamage") is dropped, see CWCraftControl.SetFallImmunity.
	if ( act && act.victim == thePlayer && !act.attacker && act.GetBuffSourceName() == "FallingDamage" )
	{
		ctl = WCraft_Control();
		if ( ctl && ctl.FallSafe() )
		{
			WCraft_ControlNote( "control: fall damage after Minecraft mode dropped" );
			return;
		}
	}
	if ( act && act.victim == thePlayer )
	{
		c = WCraft_Combat();
		if ( c )
		{
			take = c.TakesPlayerHit( act );
		}
	}
	if ( take )
	{
		c.BeginPlayerHit( act );
	}
	wrappedMethod( act );
	if ( take )
	{
		c.EndPlayerHit( act );
	}
}

@wrapMethod( W3PlayerWitcher ) function ReduceDamage( out damageData : W3DamageAction )
{
	var c : CWCraftCombat;

	c = WCraft_Combat();
	if ( c && c.CaptureReduceDamage( damageData ) )
	{
		return;
	}
	wrappedMethod( damageData );
}

exec function wccombat()
{
	var c : CWCraftCombat;
	var msg : string;

	c = WCraft_Combat();
	if ( c )
	{
		msg = c.Status();
	}
	else
	{
		msg = "WitcherCraft combat: manager not running";
	}
	LogChannel( 'WitcherCraft', msg );
	theGame.GetGuiManager().ShowNotification( msg, 8.0 );
}

// Test: send Minecraft a melee hit worth `percent` % of Geralt's max vitality.
exec function wchurt( percent : float )
{
	var maxHealth : float;
	var mc : float;

	maxHealth = thePlayer.GetStatMax( BCS_Vitality );
	mc = WCraft_CombatHurt( 0, maxHealth * percent / 100.0, maxHealth, 0, 0 );
	theGame.GetGuiManager().ShowNotification( "WitcherCraft: sent Minecraft " + FloatToStringPrec( mc, 2 ) + " damage", 3.0 );
}
