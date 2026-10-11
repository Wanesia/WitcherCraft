// WitcherCraft: Minecraft's torches, lanterns, lava, glowstone... light The Witcher's world.
//
// Minecraft lists its light-emitting blocks near the player (WcLights.java -> shared memory ->
// native WCraft_PollLights). For each one a light-only Witcher entity is spawned at the block's
// centre: the city "bounce light" the game uses to fake firelight (no model) or, if that template
// is missing, a non-interactive wall torch with every drawable (mesh, flames, decals) hidden.
// Minecraft draws the block; the Witcher only gets its light, sized by the block's light level and
// tinted by its kind. Removed when the block goes or is out of range (64 nearest within ~48 m).
// Option: Minecraft's Esc menu -> WitcherCraft... -> "Torch light in the Witcher".

import function WCraft_PollLights() : string;

class CWCraftLights
{
	private var keys : array< string >;
	private var ents : array< CEntity >;
	private var tmpl : CEntityTemplate;
	private var tmplName : string;
	private var hiddenLogged : bool;
	private var noBounce : bool;       // the bounce light had no light component: use the hidden torch
	private var lastPoll : float;
	private var warned : bool;

	public function Step()
	{
		var now : float;
		var data : string;
		var rest : string;
		var rec : string;
		var want : array< string >;
		var i : int;
		var added : int;
		var removed : int;

		now = theGame.GetEngineTimeAsSeconds();
		if ( now - lastPoll < 0.5 && now >= lastPoll )
		{
			return;
		}
		lastPoll = now;
		data = WCraft_PollLights();
		if ( data == "" )
		{
			return; // unchanged
		}
		rest = StrAfterFirst( data, ";" ); // after the version
		while ( StrLen( rest ) > 0 )
		{
			rec = StrBeforeFirst( rest, ";" );
			if ( StrLen( rec ) > 0 )
			{
				want.PushBack( rec );
			}
			rest = StrAfterFirst( rest, ";" );
		}
		for ( i = keys.Size() - 1; i >= 0; i -= 1 )
		{
			if ( !want.Contains( keys[ i ] ) )
			{
				if ( ents[ i ] )
				{
					ents[ i ].Destroy();
				}
				keys.Erase( i );
				ents.Erase( i );
				removed += 1;
			}
		}
		for ( i = 0; i < want.Size(); i += 1 )
		{
			if ( !keys.Contains( want[ i ] ) && Spawn( want[ i ] ) )
			{
				added += 1;
			}
		}
		WCraft_ControlNote( "lights: " + keys.Size() + " Minecraft lights in the Witcher world (+" + added + " -" + removed + ")" );
	}

	// rec: "x,y,z,level,kind" (W3 coordinates of the block centre)
	private function Spawn( rec : string ) : bool
	{
		var x : float;
		var y : float;
		var z : float;
		var level : int;
		var kind : int;
		var r : string;
		var ent : CEntity;
		var comps : array< CComponent >;
		var i : int;
		var light : CLightComponent;
		var draw : CDrawableComponent;
		var glc : CGameplayLightComponent;
		var hidden : int;

		x = StringToFloat( StrBeforeFirst( rec, "," ) );
		r = StrAfterFirst( rec, "," );
		y = StringToFloat( StrBeforeFirst( r, "," ) );
		r = StrAfterFirst( r, "," );
		z = StringToFloat( StrBeforeFirst( r, "," ) );
		r = StrAfterFirst( r, "," );
		level = StringToInt( StrBeforeFirst( r, "," ) );
		kind = StringToInt( StrAfterFirst( r, "," ) );

		if ( !tmpl && !noBounce )
		{
			tmplName = "city_bouncelight_fire";
			tmpl = (CEntityTemplate)LoadResource( "environment\decorations\light_sources\city_bouncelights\city_bouncelight_fire.w2ent", true );
		}
		if ( !tmpl )
		{
			tmplName = "torch_wall_wooden_small_non_interactive";
			tmpl = (CEntityTemplate)LoadResource( "environment\decorations\light_sources\torches\torch_wall_wooden_small_non_interactive.w2ent", true );
		}
		if ( !tmpl )
		{
			if ( !warned )
			{
				warned = true;
				WCraft_ControlNote( "lights: the torch template is missing" );
			}
			return false;
		}
		ent = theGame.CreateEntity( tmpl, Vector( x, y, z, 1.0 ), EulerAngles( 0, 0, 0 ), false, false, true, PM_DontPersist );
		if ( !ent )
		{
			return false;
		}
		// Lit, and not something Geralt can put out.
		glc = (CGameplayLightComponent)ent.GetComponentByClassName( 'CGameplayLightComponent' );
		if ( glc )
		{
			glc.SetLight( true );
			glc.SetInteractive( false );
		}
		// Only the light: Minecraft draws the block itself. Every drawable goes (meshes of any kind,
		// flame particles, decals); lights are not drawables (CLightComponent is a CSpriteComponent).
		comps = ent.GetComponentsByClassName( 'CDrawableComponent' );
		for ( i = 0; i < comps.Size(); i += 1 )
		{
			draw = (CDrawableComponent)comps[ i ];
			if ( draw )
			{
				draw.SetVisible( false );
				hidden += 1;
			}
		}
		comps = ent.GetComponentsByClassName( 'CParticleComponent' );
		for ( i = 0; i < comps.Size(); i += 1 )
		{
			draw = (CDrawableComponent)comps[ i ];
			if ( draw )
			{
				draw.SetVisible( false );
				hidden += 1;
			}
		}
		ent.StopAllEffects();
		comps = ent.GetComponentsByClassName( 'CLightComponent' );
		if ( comps.Size() == 0 && !noBounce && tmplName == "city_bouncelight_fire" )
		{
			WCraft_ControlNote( "lights: the bounce light has no light component; using the hidden torch" );
			ent.Destroy();
			noBounce = true;
			tmpl = NULL;
			return Spawn( rec );
		}
		if ( !hiddenLogged )
		{
			hiddenLogged = true;
			WCraft_ControlNote( "lights: template " + tmplName + ", " + hidden + " drawable(s) hidden, " + comps.Size() + " light(s)" );
		}
		for ( i = 0; i < comps.Size(); i += 1 )
		{
			light = (CLightComponent)comps[ i ];
			if ( !light )
			{
				continue;
			}
			light.radius = 0.7 * level;
			if ( kind == 1 )
			{
				light.color = Color( 90, 200, 255 );
			}
			else if ( kind == 2 )
			{
				light.color = Color( 255, 120, 40 );
				light.brightness = light.brightness * 1.5;
			}
			else if ( kind == 3 )
			{
				light.color = Color( 255, 235, 200 );
			}
			else if ( kind == 4 )
			{
				light.color = Color( 255, 40, 30 );
				light.brightness = light.brightness * 0.5;
			}
			light.SetEnabled( false ); // re-create its render proxy with the new values
			light.SetEnabled( true );
		}
		keys.PushBack( rec );
		ents.PushBack( ent );
		return true;
	}

	public function Clear()
	{
		var i : int;

		for ( i = 0; i < ents.Size(); i += 1 )
		{
			if ( ents[ i ] )
			{
				ents[ i ].Destroy();
			}
		}
		keys.Clear();
		ents.Clear();
	}
}
