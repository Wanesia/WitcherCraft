// WitcherCraft M3a: Minecraft mode. Minecraft drives the player.
//
// Every frame (latent loop with SleepOneFrame):
//   - WCraft_ControlFrame tells the native side whether The Witcher needs the player right now
//     (scene, dialogue, horse, death, fade: "suspend") or the keyboard (a menu: "no capture"),
//     and returns Minecraft's interpolated eye. While Minecraft drives:
//       * the camera: the photo-mode free camera (a CCustomCamera that exists in every game),
//         placed at Minecraft's eye with Minecraft's look every frame (SetCameraWorldPosition /
//         SetCameraWorldRotation / UpdateWithoutInput, as photomodeMenu.ws blends it per frame)
//       * Geralt: hidden (SetHideInGame), gravity off, Witcher actions blocked, and teleported to
//         Minecraft's feet so streaming, NPC AI and quests follow
//   - the native input hook (control.cpp) sends keys / mouse to Minecraft and hides them from W3
// F8 (native) toggles back to Witcher mode (Geralt drives, Minecraft mirrors him, F7 build mode).
// M6a mode identity: Minecraft mode = Minecraft's identity (Esc = Minecraft's
// menu, The Witcher's HUD hidden: witchercraft_hud.ws; M/J/I/K open the Witcher's map / journal /
// inventory / character panel over Minecraft mode, Steve stays as he is: PanelFrame; N switches
// to Geralt to meditate and comes back after); Witcher mode = pure Witcher (no Minecraft overlay). Hidden Geralt always stands on
// the ground (or water surface) under Steve, gravity on, so W3 never sees him in the air: saves
// are allowed and nothing falls when Minecraft mode lets go.
// Console: wcsave()  wchud( 0|1 )  wcmode()  wcmode2( true|false )  wccam( 0|1 )  wcfov( 0|1|2 )  wccamsrc( 0|1 )  wcctl()
//          wcsync()  wclat( -1|0..8 )  wcmatch( 0|1 )   (visual sync)

// Visual sync: status / latency knob, and warnings (frame generation, motion blur,
// Minecraft slower than The Witcher) to show as notifications.
import function WCraft_SyncOption( latency : int, frameMatch : int ) : string;
import function WCraft_SyncWarning() : string;
// M6a: the Witcher panel key (DIK) that just switched Minecraft mode -> Witcher mode (0 none, -1 McIdentity=0).
import function WCraft_ModeRequest() : int;
// The Witcher's panels as Steve: op 1 = opened (Geralt at pos), 0 = closed, 2 = query. Returns
// 1 panel state on, 2 the script was paused meanwhile (a menu was up), 4 opened.
import function WCraft_Panel( op : int, pos : Vector ) : int;
// M7: V held in Minecraft mode = Witcher senses (focus mode).
import function WCraft_SensesHeld() : bool;
import function WCraft_KeyName( which : int ) : string;

class CWCraftControl
{
	private var engaged : bool;
	private var hidden : bool;
	private var blocked : bool;
	private var cam : CCustomCamera;      // only for the wccam(1) game-camera fallback
	private var scam : CStaticCamera;     // default: a spawned static camera we move every frame
	private var yawOffset : float;        // self-calibrated: 0 or 180 if the entity faces backwards
	private var yawVotes : int;
	private var pitchSign : float;
	private var flipVotes : int;
	private var lastPitchCmd : float;
	private var lastReport : float;
	private var lastMode : bool;
	private var modeKnown : bool;
	private var wantTeleport : bool;
	private var useGameCamera : bool;
	private var notified : bool;
	private var frames : int;
	private var lastSyncPoll : float;
	private var interact : CWCraftInteract; // the G key (witchercraft_interact.ws)
	private var faceHeading : float;        // interact: hidden Geralt turned towards a target ...
	private var faceUntil : float;          // ... until this engine time
	private var invisible : bool;           // SetVisibility(false) as well (no shadow), see Engage
	private var hideMode : int;             // wchide(n): 1 SetHideInGame only, 2 + SetVisibility (default)
	// M6a
	private var hud : CWCraftHud;           // The Witcher's HUD hidden while Minecraft drives
	private var pendingPanel : int;         // Witcher panel to open once Geralt is back (DIK)
	private var pendingFrames : int;
	private var placed : Vector;            // where we last put Geralt
	private var placedValid : bool;
	private var ground : Vector;            // the last ground / water surface found under Steve
	private var groundValid : bool;
	private var groundKind : string;        // "ground", "water", "kept" (no ground under Steve), "none"
	private var followPauseUntil : float;   // after a W3-side teleport: let Minecraft come to Geralt
	private var lastFollow : float;
	private var fallSafeUntil : float;      // the game's fall damage on Geralt is dropped until then
	private var lastSaveCheck : float;
	private var saveDiag : string;
	private var traceGroups : array< name >;
	private var sensesOn : bool;            // M7: focus mode turned on by us (V held)
	// The Witcher's panels as Steve
	private var panelKey : int;             // the panel shown over Minecraft mode (DIK), 0 = none
	private var panelFrames : int;
	private var panelOpened : bool;
	private var panelMenuSeen : bool;
	private var panelClear : int;           // frames without a Witcher menu once it was up
	private var medReturn : bool;           // N: back to Minecraft mode when the meditation is over
	private var medSeen : bool;
	private var medFrames : int;

	public function Init()
	{
		if ( pitchSign == 0.0 )
		{
			pitchSign = 1.0;
		}
		engaged = false;
		frames = 0;
		panelKey = 0;
		ClearOldSafetyNet();
		WCraft_Panel( 0, Vector( 0, 0, 0, 1 ) ); // a panel state left over from before a load
		WCraft_ControlNote( "control: ready" );
	}

	// The Witcher wants the player (no Minecraft driving).
	private function SuspendReason() : string
	{
		if ( !thePlayer )
		{
			return "no player";
		}
		if ( theGame.IsLoadingScreenVideoPlaying() )
		{
			return "loading";
		}
		if ( theGame.IsDialogOrCutscenePlaying() || theGame.IsCurrentlyPlayingNonGameplayScene() || thePlayer.IsInNonGameplayCutscene() )
		{
			return "scene";
		}
		if ( theGame.IsBlackscreenOrFading() )
		{
			return "fade";
		}
		if ( thePlayer.IsUsingVehicle() )
		{
			return "vehicle";
		}
		if ( !thePlayer.IsAlive() )
		{
			return "dead";
		}
		return "";
	}

	public function Frame()
	{
		var flags : int;
		var dir : Vector;
		var horiz : float;
		var camPitch : float;
		var eye : Vector;
		var on : bool;
		var mode : bool;
		var reason : string;
		var now : float;
		var warn : string;
		var n : int;

		if ( !thePlayer )
		{
			return;
		}
		frames += 1;
		now = theGame.GetEngineTimeAsSeconds();
		if ( now - lastSyncPoll > 2.0 || now < lastSyncPoll )
		{
			lastSyncPoll = now;
			warn = WCraft_SyncWarning();
			if ( warn != "" )
			{
				theGame.GetGuiManager().ShowNotification( "WitcherCraft: " + warn, 10.0 );
				WCraft_ControlNote( "sync warning shown: " + warn );
			}
		}
		reason = SuspendReason();
		flags = 0;
		if ( reason != "" )
		{
			flags += 1;
		}
		if ( theGame.GetGuiManager().IsAnyMenu() || IsInputPopupOpen() )
		{
			flags += 2;
		}
		if ( engaged )
		{
			flags += 4; // Geralt is kept on the ground under Steve (status line)
		}
		dir = theCamera.GetCameraDirection();
		horiz = SqrtF( dir.X * dir.X + dir.Y * dir.Y );
		camPitch = Rad2Deg( AtanF( dir.Z, horiz ) );
		eye = WCraft_ControlFrame( flags, theCamera.GetCameraHeading(), camPitch );
		on = eye.W > 0.5;

		mode = WCraft_ControlMode();
		if ( !modeKnown || mode != lastMode )
		{
			if ( modeKnown )
			{
				ShowModeHint( mode );
			}
			modeKnown = true;
			lastMode = mode;
		}
		n = WCraft_ModeRequest();
		if ( n == 49 )
		{
			pendingPanel = n; // N: meditation as Geralt (the native side switched to Witcher mode)
			pendingFrames = 0;
		}
		else if ( n > 0 )
		{
			panelKey = n;     // M/J/I/K: that panel over Minecraft mode, Steve stays as he is
			panelFrames = 0;
			panelOpened = false;
			panelMenuSeen = false;
			panelClear = 0;
		}

		if ( on && !engaged )
		{
			Engage();
		}
		else if ( !on && engaged )
		{
			Disengage( reason );
		}
		if ( engaged )
		{
			if ( eye.W > 0.5 && eye.W < 1.5 )
			{
				PlaceCamera( eye );
			}
			FollowBody();
		}
		else if ( reason == "" && !theGame.GetGuiManager().IsAnyMenu() && cam && theCamera.GetTopmostCamera() == cam && !useGameCamera )
		{
			// Our camera was left on top (e.g. a scene ended back into it): hand back the game camera.
			theGame.GetGameCamera().Activate( 0.0, false );
		}

		// M6a: The Witcher's HUD (hidden while Minecraft drives), the panel a Witcher key asked for,
		// the fall-damage safety net and the save-lock diagnostics.
		if ( !hud )
		{
			hud = new CWCraftHud in this;
		}
		hud.Frame( engaged );
		if ( pendingPanel > 0 )
		{
			pendingFrames += 1;
			if ( mode || pendingFrames > 600 )
			{
				pendingPanel = 0; // back in Minecraft mode, or never got a free moment
			}
			else if ( !engaged && reason == "" && pendingFrames >= 3 && !theGame.GetGuiManager().IsAnyMenu() )
			{
				OpenPanel( pendingPanel );
				if ( pendingPanel == 49 )
				{
					medReturn = true;
					medSeen = false;
					medFrames = 0;
				}
				pendingPanel = 0;
			}
		}
		PanelFrame( mode, reason );
		MeditationFrame( mode, reason );
		if ( now - lastSaveCheck > 2.0 || now < lastSaveCheck )
		{
			lastSaveCheck = now;
			warn = SaveDiagnostics();
			if ( warn != saveDiag )
			{
				saveDiag = warn;
				WCraft_ControlNote( "save: " + warn );
			}
		}

		// The W3 interact key (G).
		if ( !interact )
		{
			interact = new CWCraftInteract in this;
		}
		interact.Frame( this, engaged, reason == "" && ( flags & 2 ) == 0 ); // 2 = a W3 menu / popup
		SensesFrame( engaged );
	}

	// M7: Witcher senses as Steve. Hold V (native: WCraft_SensesHeld) = The Witcher's focus mode
	// (clues, tracks and loot glow; G examines them), as the right mouse button does for Geralt.
	private function SensesFrame( on : bool )
	{
		var fmc : CFocusModeController;
		var held : bool;

		held = on && WCraft_SensesHeld();
		if ( !held && !sensesOn )
		{
			return;
		}
		fmc = theGame.GetFocusModeController();
		if ( !fmc )
		{
			return;
		}
		if ( held && !sensesOn )
		{
			// Once per press: Activate() (re)starts the controller's short fast-focus timer.
			fmc.Activate();
			sensesOn = true;
			WCraft_ControlNote( "control: Witcher senses ON (V held)" );
		}
		else if ( !held && sensesOn )
		{
			if ( fmc.IsActive() )
			{
				fmc.Deactivate();
			}
			sensesOn = false;
			WCraft_ControlNote( "control: Witcher senses OFF" );
		}
	}

	// W3 popups that need The Witcher's keyboard and mouse (loot window, item choice, message box).
	private function IsInputPopupOpen() : bool
	{
		var gm : CR4GuiManager;

		gm = theGame.GetGuiManager();
		if ( gm.GetPopup( 'LootPopup' ) )
		{
			return true;
		}
		if ( gm.GetPopup( 'ItemSelectionPopup' ) )
		{
			return true;
		}
		if ( gm.GetPopup( 'MessagePopup' ) )
		{
			return true;
		}
		return false;
	}

	// Interact: hidden Geralt faces this heading for a moment instead of Minecraft's look.
	public function FaceHeading( heading : float, seconds : float )
	{
		var feet : Vector;

		faceHeading = heading;
		faceUntil = theGame.GetEngineTimeAsSeconds() + seconds;
		feet = thePlayer.GetWorldPosition();
		thePlayer.TeleportWithRotation( feet, EulerAngles( 0, heading, 0 ) );
	}

	public function InteractStatus() : string
	{
		if ( interact )
		{
			return interact.Status();
		}
		return "interact: not started";
	}

	public function SetHideMode( mode : int )
	{
		hideMode = mode;
		if ( engaged )
		{
			ApplyHidden( true );
		}
	}

	// Hidden Geralt: SetHideInGame (as poster.ws) plus, by default, CActor.SetVisibility(false) (as
	// photo mode hides characters), which also takes his shadow and hair away.
	private function ApplyHidden( on : bool )
	{
		if ( hideMode == 0 )
		{
			hideMode = 2;
		}
		if ( on )
		{
			thePlayer.SetHideInGame( true );
			hidden = true;
			if ( hideMode >= 2 && !invisible )
			{
				thePlayer.SetVisibility( false );
				invisible = true;
			}
			else if ( hideMode < 2 && invisible )
			{
				thePlayer.SetVisibility( true );
				invisible = false;
			}
		}
		else
		{
			if ( hidden )
			{
				thePlayer.SetHideInGame( false );
				hidden = false;
			}
			if ( invisible )
			{
				thePlayer.SetVisibility( true );
				invisible = false;
			}
		}
	}

	private function Engage()
	{
		var mac : CMovingPhysicalAgentComponent;

		engaged = true;
		flipVotes = 0;
		SetActionsBlocked( true );
		ApplyHidden( true );
		// M6a: gravity stays ON. Geralt is kept on the ground under Steve (FollowBody), so W3 sees a
		// grounded player (saves allowed) and nothing falls when Minecraft mode lets go.
		mac = (CMovingPhysicalAgentComponent)thePlayer.GetMovingAgentComponent();
		if ( mac )
		{
			mac.SetGravity( true );
		}
		placedValid = false;
		lastFollow = theGame.GetEngineTimeAsSeconds();
		// The photo-mode camera must NOT be used: activating it outside photo mode pauses the game and
		// crashes 5.0. Default: a spawned static camera (the kind posters/quests use).
		cam = NULL;
		if ( !useGameCamera )
		{
			EnsureStaticCamera();
			if ( !scam )
			{
				WCraft_ControlNote( "control: static camera unavailable; using the game camera" );
				useGameCamera = true;
			}
		}
		if ( useGameCamera )
		{
			cam = theGame.GetGameCamera();
			if ( !cam )
			{
				WCraft_ControlNote( "control: NO CAMERA (GetGameCamera returned NULL)" );
			}
		}
		if ( !notified )
		{
			notified = true;
			ShowModeHint( true );
		}
		WCraft_ControlNote( "control: engaged (camera " + useGameCamera + " game camera, player hidden, actions blocked)" );
	}

	private function EnsureStaticCamera()
	{
		var tmpl : CEntityTemplate;
		var comps : array< CComponent >;
		var more : array< CComponent >;
		var i : int;

		if ( scam )
		{
			return;
		}
		tmpl = (CEntityTemplate)LoadResource( "gameplay\poster\poster_camera_top_view.w2ent", true );
		if ( !tmpl )
		{
			WCraft_ControlNote( "control: static camera template not found" );
			return;
		}
		scam = (CStaticCamera)theGame.CreateEntity( tmpl, thePlayer.GetWorldPosition(), thePlayer.GetWorldRotation(), false, false, true, PM_DontPersist );
		if ( scam )
		{
			// The template's camera component sits ~1 m up and tilted down ("top view"): zero every
			// component's local transform so the entity transform IS the camera transform.
			comps = scam.GetComponentsByClassName( 'CCameraComponent' );
			more = scam.GetComponentsByClassName( 'CComponent' );
			for ( i = 0; i < more.Size(); i += 1 )
			{
				comps.PushBack( more[ i ] );
			}
			for ( i = 0; i < comps.Size(); i += 1 )
			{
				comps[ i ].SetPosition( Vector( 0, 0, 0 ) );
				comps[ i ].SetRotation( EulerAngles( 0, 0, 0 ) );
			}
			WCraft_ControlNote( "control: static camera components zeroed: " + comps.Size() );
			scam.activationDuration = 0.0;
			scam.deactivationDuration = 0.0;
			scam.fadeStartDuration = 0.0;
			scam.fadeEndDuration = 0.0;
			scam.timeout = 0.0;
			WCraft_ControlNote( "control: static camera spawned" );
		}
		else
		{
			WCraft_ControlNote( "control: static camera CreateEntity failed" );
		}
	}

	private function Disengage( reason : string )
	{
		var mac : CMovingPhysicalAgentComponent;

		engaged = false;
		// M6a: Geralt must be standing before W3 gets him back (never dropped from a height), and
		// immune to fall damage for a moment in case anything still goes wrong.
		if ( reason != "loading" && reason != "dead" && reason != "vehicle" && thePlayer.IsAlive() )
		{
			GroundNow( reason );
			SetFallImmunity( 4.0 );
		}
		SetActionsBlocked( false );
		ApplyHidden( false );
		mac = (CMovingPhysicalAgentComponent)thePlayer.GetMovingAgentComponent();
		if ( mac )
		{
			mac.SetGravity( true );
		}
		// A scene / dialogue brings its own camera: only hand the view back to the game camera when
		// nothing else took it (toggle, link lost).
		if ( scam && scam.IsRunning() )
		{
			scam.Stop();
		}
		if ( reason != "" )
		{
			// Whatever moved Geralt meanwhile (a scene, a horse ride), Minecraft goes there after.
			wantTeleport = true;
		}
		WCraft_ControlNote( "control: disengaged (" + reason + ")" );
	}


	// Minecraft's eye and look -> the W3 camera. The pitch sign of the camera's EulerAngles is
	// verified against the camera's real direction and flipped once if it is the other way round.
	private function PlaceCamera( eye : Vector )
	{
		var look : Vector;
		var rot : EulerAngles;
		var dir : Vector;
		var now : float;
		var real : Vector;
		var want : Vector;

		if ( !cam && !scam )
		{
			return;
		}
		if ( cam && theCamera.GetTopmostCamera() != cam )
		{
			cam.Activate( 0.0, false );
		}
		if ( scam && !useGameCamera && !scam.IsRunning() )
		{
			scam.Run();
		}
		look = WCraft_ControlLook();

		// Self-calibration: last frame we asked for lastPitchCmd (+ = up); is the camera looking that way?
		dir = theCamera.GetCameraDirection();
		if ( AbsF( lastPitchCmd ) > 15.0 && AbsF( dir.Z ) > 0.15 )
		{
			if ( ( lastPitchCmd > 0.0 ) != ( dir.Z > 0.0 ) )
			{
				flipVotes += 1;
				if ( flipVotes >= 5 )
				{
					pitchSign = -pitchSign;
					flipVotes = 0;
					WCraft_ControlNote( "control: camera pitch sign flipped to " + FloatToString( pitchSign ) );
				}
			}
			else
			{
				flipVotes = 0;
			}
		}

		// Same for heading: if the camera looks the opposite way of what we asked, add 180 once.
		want = VecFromHeading( look.Y );
		if ( AbsF( dir.Z ) < 0.9 )
		{
			if ( want.X * dir.X + want.Y * dir.Y < -0.3 )
			{
				yawVotes += 1;
				if ( yawVotes >= 5 )
				{
					yawOffset = 180.0 - yawOffset;
					yawVotes = 0;
					WCraft_ControlNote( "control: camera yaw offset now " + FloatToString( yawOffset ) );
				}
			}
			else
			{
				yawVotes = 0;
			}
		}

		rot = EulerAngles( pitchSign * look.X, look.Y + yawOffset, look.Z );
		// look.W = Minecraft's vertical FOV already converted to W3's convention natively
		// (CameraFovMode in the ini / wcfov(n), real back-buffer aspect); 0 = unknown.
		if ( scam && !useGameCamera )
		{
			scam.TeleportWithRotation( eye, rot );
			if ( look.W > 10.0 )
			{
				scam.SetFov( look.W );
			}
		}
		else
		{
			cam.SetCameraWorldPosition( eye );
			cam.SetCameraWorldRotation( rot );
			if ( look.W > 10.0 )
			{
				cam.SetFov( look.W );
			}
			cam.UpdateWithoutInput( true );
		}
		lastPitchCmd = look.X;

		now = theGame.GetEngineTimeAsSeconds();
		if ( now - lastReport > 5.0 )
		{
			lastReport = now;
			real = theCamera.GetCameraPosition();
			WCraft_ControlNote( "control: camera want " + VecToStringPrec( eye, 2 ) + " hdg " + FloatToStringPrec( look.Y, 1 ) + " pitch " + FloatToStringPrec( look.X, 1 )
				+ " | real " + VecToStringPrec( real, 2 ) + " hdg " + FloatToStringPrec( theCamera.GetCameraHeading(), 1 ) + " dirZ " + FloatToStringPrec( dir.Z, 2 )
				+ " | off " + FloatToStringPrec( VecDistance( real, eye ), 2 ) + " m | frames " + frames );
		}
	}

	// M6a: hidden Geralt stands on the GROUND (or the water surface) under Minecraft's player, with
	// gravity on, facing the player's way (not the camera's: Minecraft's F5 front view looks back
	// at Steve), or towards an interact target. Over a void (no ground found) he keeps the last
	// ground. He never moves more than ~2 m per 33 ms host step: the host re-places Minecraft on
	// Geralt when he moves more than 6 m in one step while more than 3 m from Steve (its "W3 moved
	// its player" rule), so long moves (Steve flying over a cliff or a roof) go through the air at
	// up to 60 m/s: up first, then across, then down. Teleports reset W3's fall height.
	private function FollowBody()
	{
		var feet : Vector;
		var body : Vector;
		var here : Vector;
		var target : Vector;
		var surf : Vector;
		var next : Vector;
		var heading : float;
		var now : float;
		var dt : float;
		var budget : float;
		var d : float;
		var mv : float;

		now = theGame.GetEngineTimeAsSeconds();
		dt = now - lastFollow;
		lastFollow = now;
		if ( dt < 0.0 || dt > 0.05 )
		{
			dt = 0.05;
		}
		here = thePlayer.GetWorldPosition();

		// The Witcher moved Geralt by itself (a quest teleport, fast travel without a fade):
		// Minecraft goes there (kTickTeleport), and we leave Geralt alone until it has.
		if ( placedValid && VecDistance2D( here, placed ) > 8.0 )
		{
			WCraft_ControlNote( "control: The Witcher moved Geralt " + FloatToStringPrec( VecDistance2D( here, placed ), 1 ) + " m by itself; Minecraft follows him there" );
			wantTeleport = true;
			placed = here;
			ground = here;
			groundValid = true;
			groundKind = "w3 teleport";
			followPauseUntil = now + 1.5;
			return;
		}
		if ( now < followPauseUntil )
		{
			return;
		}

		feet = WCraft_ControlFeet();
		if ( feet.W < 0.5 )
		{
			return;
		}
		feet.W = 1.0;
		body = WCraft_ControlBody();
		heading = body.X;
		if ( body.W < 0.5 )
		{
			body = WCraft_ControlLook(); // struct fields can't be read off a call result directly
			heading = body.Y;
		}
		if ( now < faceUntil )
		{
			heading = faceHeading;
		}

		if ( TraceSurface( feet, surf ) )
		{
			ground = surf;
			groundValid = true;
		}
		else if ( groundValid )
		{
			groundKind = "kept";
		}
		if ( groundValid )
		{
			target = ground;
			target.Z += 0.05;
		}
		else
		{
			target = here; // nothing found yet: stay where The Witcher put him
			groundKind = "none";
		}
		target.W = 1.0;

		// At most 60 m/s (about 2 m per 33 ms host step): up first, then across, then down.
		budget = 60.0 * dt;
		next = here;
		next.W = 1.0;
		if ( VecDistance( here, target ) <= MaxF( budget, 0.5 ) )
		{
			next = target;
		}
		else
		{
			if ( target.Z > next.Z + 0.3 && VecDistance2D( next, target ) > 0.3 )
			{
				mv = MinF( budget, target.Z + 0.3 - next.Z );
				next.Z += mv;
				budget -= mv;
			}
			d = VecDistance2D( next, target );
			if ( budget > 0.0 && d > 0.001 )
			{
				mv = MinF( budget, d );
				next.X += ( target.X - next.X ) * mv / d;
				next.Y += ( target.Y - next.Y ) * mv / d;
				budget -= mv;
			}
			if ( budget > 0.0 && VecDistance2D( next, target ) < 0.01 )
			{
				mv = MinF( budget, AbsF( target.Z - next.Z ) );
				if ( target.Z < next.Z )
				{
					next.Z -= mv;
				}
				else
				{
					next.Z += mv;
				}
			}
		}
		if ( VecDistance( here, next ) > 0.15 || AbsF( AngleDistance( thePlayer.GetHeading(), heading ) ) > 10.0 )
		{
			thePlayer.TeleportWithRotation( next, EulerAngles( 0, heading, 0 ) );
			placed = next;
		}
		else if ( !placedValid )
		{
			placed = here;
		}
		placedValid = true;
	}

	// The ground under a point: the first static surface (terrain, buildings, platforms) below
	// point + 0.6 m; the water surface instead when the ground there is under water.
	private function TraceSurface( at : Vector, out surf : Vector ) : bool
	{
		var a : Vector;
		var b : Vector;
		var hit : Vector;
		var normal : Vector;
		var wl : float;

		if ( traceGroups.Size() == 0 )
		{
			traceGroups.PushBack( 'Terrain' );
			traceGroups.PushBack( 'Static' );
			traceGroups.PushBack( 'Platforms' );
			traceGroups.PushBack( 'Destructible' );
		}
		a = at;
		a.Z += 0.6;
		a.W = 1.0;
		b = a;
		b.Z -= 500.0;
		if ( !theGame.GetWorld().StaticTrace( a, b, hit, normal, traceGroups ) )
		{
			return false;
		}
		surf = hit;
		surf.W = 1.0;
		groundKind = "ground";
		wl = theGame.GetWorld().GetWaterLevel( hit );
		if ( wl > hit.Z + 0.3 && wl < hit.Z + 500.0 )
		{
			surf.Z = wl; // Geralt swims at the surface instead of standing (drowning) on the bottom
			groundKind = "water";
		}
		return true;
	}

	// Before W3 gets Geralt back: if he is above the ground (or water) under him, put him on it.
	private function GroundNow( why : string )
	{
		var here : Vector;
		var surf : Vector;
		var heading : float;

		here = thePlayer.GetWorldPosition();
		heading = thePlayer.GetHeading();
		if ( TraceSurface( here, surf ) )
		{
			if ( here.Z - surf.Z > 0.4 )
			{
				surf.Z += 0.05;
				thePlayer.TeleportWithRotation( surf, EulerAngles( 0, heading, 0 ) );
				WCraft_ControlNote( "control: grounded Geralt before handing him back (" + why + "): " + FloatToStringPrec( here.Z - surf.Z, 2 ) + " m down" );
			}
		}
		else if ( groundValid && here.Z - ground.Z > 0.4 )
		{
			surf = ground;
			surf.Z += 0.05;
			thePlayer.TeleportWithRotation( surf, EulerAngles( 0, heading, 0 ) );
			WCraft_ControlNote( "control: no ground under Geralt; put him on the last ground (" + why + ")" );
		}
	}

	// Fall-damage safety net for mode transitions: the game's fall damage on Geralt is dropped for a
	// few seconds (W3DamageManager.ProcessAction wrapper, witchercraft_combat.ws). Nothing of it is
	// saved, unlike an immortality mode (actor.ws immortalityFlags is a saved field).
	private function SetFallImmunity( seconds : float )
	{
		fallSafeUntil = theGame.GetEngineTimeAsSeconds() + seconds;
	}

	// 0.1.0 used an unlocked AIC_Cheat invulnerability as the fall-damage safety net, and the game
	// saves it: clear it from saves made with that build. The game's own god mode locks the channel,
	// and an unlocked call never changes a locked channel (actor.ws SetImmortalityMode). Called at
	// manager start and again once the loading screen is gone.
	public function ClearOldSafetyNet()
	{
		if ( thePlayer )
		{
			thePlayer.SetImmortalityMode( AIM_None, AIC_Cheat );
		}
	}

	public function FallSafe() : bool
	{
		var now : float;

		now = theGame.GetEngineTimeAsSeconds();
		return now < fallSafeUntil && now > fallSafeUntil - 60.0;
	}

	// Why The Witcher won't save. The script API only says whether ANY no-save lock is set, so the
	// usual suspects are listed: the exploration state (falling / jumping / sliding lock saves),
	// combat, and where we keep Geralt.
	public function SaveDiagnostics() : string
	{
		var sm : CExplorationStateManager;
		var s : string;

		s = "locked " + theGame.AreSavesLocked();
		sm = thePlayer.substateManager;
		if ( sm )
		{
			s += " | exploration " + NameToString( sm.GetStateCur() ) + " onGround " + sm.IsOnGround();
		}
		s += " | combat " + thePlayer.IsInCombat() + " | alive " + thePlayer.IsAlive() + " | Minecraft drives " + engaged + " | Geralt " + groundKind;
		return s;
	}

	public function TryUnlockSave() : string
	{
		var sm : CExplorationStateManager;
		var before : bool;

		before = theGame.AreSavesLocked();
		sm = thePlayer.substateManager;
		if ( before && sm && sm.IsOnGround() && !thePlayer.IsInCombat() )
		{
			// The exploration lock is stale when Geralt stands on the ground (the exploration
			// manager re-creates it itself if he really is falling).
			theGame.ReleaseNoSaveLockByName( "exploration_state" );
			return "released the exploration lock: locked " + before + " -> " + theGame.AreSavesLocked();
		}
		return "nothing released (locked " + before + ")";
	}

	// The Witcher panel for a panel key, as vanilla playerInput.ws opens them (they show The
	// Witcher's own "not now" message when the action is blocked). False = it can't open now.
	private function OpenPanel( dik : int ) : bool
	{
		var ih : CPlayerInput;
		var allowed : bool;

		ih = thePlayer.GetInputHandler();
		if ( !ih || theGame.IsBlackscreenOrFading() )
		{
			return false;
		}
		WCraft_ControlNote( "control: opening The Witcher's panel for key " + dik );
		allowed = true;
		if ( dik == 50 )
		{
			allowed = thePlayer.IsActionAllowed( EIAB_OpenMap );
			ih.PushMapScreen();          // M
		}
		else if ( dik == 36 )
		{
			allowed = thePlayer.IsActionAllowed( EIAB_OpenJournal );
			ih.PushJournalScreen();      // J
		}
		else if ( dik == 23 )
		{
			allowed = thePlayer.IsActionAllowed( EIAB_OpenInventory );
			ih.PushInventoryScreen();    // I
		}
		else if ( dik == 37 )
		{
			allowed = thePlayer.IsActionAllowed( EIAB_OpenCharacterPanel );
			ih.PushCharacterScreen();    // K
		}
		else if ( dik == 49 )
		{
			ih.StartMeditation();        // N
		}
		else
		{
			allowed = false;
		}
		return allowed;
	}

	// The Witcher's panel over Minecraft mode (M/J/I/K). The key
	// press already gave the input to The Witcher and paused Minecraft (native). Here: open the panel
	// once with Geralt as he is (hidden, on the ground under Steve; nothing is switched or moved),
	// tell the native side where he stands (it ticks the host for us while the menu pauses this
	// script), and end the panel state once The Witcher's menu has come and gone: Minecraft then
	// resumes by itself, Steve exactly where he was (also mid-air, flying or swimming).
	private function PanelFrame( mode : bool, reason : string )
	{
		var st : int;
		var menu : bool;

		if ( panelKey <= 0 )
		{
			return;
		}
		panelFrames += 1;
		if ( !panelOpened )
		{
			if ( !mode || reason != "" || !engaged )
			{
				EndPanel( "Minecraft isn't driving now (" + reason + ")" );
				return;
			}
			WCraft_Panel( 1, thePlayer.GetWorldPosition() );
			panelOpened = true;
			if ( !OpenPanel( panelKey ) )
			{
				EndPanel( "The Witcher won't open that panel now" );
			}
			return;
		}
		st = WCraft_Panel( 2, thePlayer.GetWorldPosition() );
		if ( ( st & 1 ) == 0 )
		{
			WCraft_ControlNote( "control: Witcher panel " + panelKey + " ended natively" );
			panelKey = 0;
			return;
		}
		menu = theGame.GetGuiManager().IsAnyMenu();
		if ( menu )
		{
			panelMenuSeen = true;
			panelClear = 0;
			return;
		}
		// Closed: the menu was seen (or it paused this script), or it never came (2 s).
		if ( panelMenuSeen || ( st & 2 ) != 0 || panelFrames > 120 )
		{
			panelClear += 1;
			if ( panelClear >= 3 )
			{
				EndPanel( "closed (menu seen " + panelMenuSeen + ", script paused " + ( ( st & 2 ) != 0 ) + ")" );
			}
		}
	}

	private function EndPanel( why : string )
	{
		WCraft_Panel( 0, thePlayer.GetWorldPosition() );
		WCraft_ControlNote( "control: Witcher panel " + panelKey + " over Minecraft mode ended: " + why );
		panelKey = 0;
	}

	// N: Geralt meditates (he sits down while time passes, so that runs in Witcher mode). Once the
	// meditation is over (or it was refused), Steve comes back by himself where Geralt sat.
	private function MeditationFrame( mode : bool, reason : string )
	{
		var st : name;

		if ( !medReturn )
		{
			return;
		}
		if ( mode )
		{
			medReturn = false; // already back (the switch key)
			return;
		}
		st = thePlayer.GetCurrentStateName();
		if ( st == 'Meditation' || st == 'MeditationWaiting' )
		{
			medSeen = true;
			medFrames = 0;
			return;
		}
		if ( reason != "" || theGame.GetGuiManager().IsAnyMenu() )
		{
			return;
		}
		medFrames += 1;
		if ( ( medSeen && medFrames >= 20 ) || medFrames >= 120 )
		{
			medReturn = false;
			WCraft_ControlNote( "control: meditation over (meditated " + medSeen + "); back to Minecraft mode" );
			WCraft_SetControlMode( true );
		}
	}

	private function ShowModeHint( mc : bool )
	{
		var o : int;
		var keysLine : string;
		var panels : string;

		o = WCraft_Options();
		if ( o > 0 && ( o & 64 ) == 0 )
		{
			return; // "Key hints on mode switch" off in Minecraft's WitcherCraft options
		}
		// The keys as Minecraft's WitcherCraft controls set them (unbound ones are left out).
		if ( mc )
		{
			keysLine = KeyHint( 0, "talk, loot, doors, mount Roach", "" ) + KeyHint( 2, "Witcher senses", "hold " ) + KeyHint( 3, "call Roach", "" );
			panels = "";
			if ( o <= 0 || ( o & 512 ) != 0 )
			{
				panels = "M map, J journal, I Witcher inventory, K character (as Steve) | N: meditate as Geralt<br>";
			}
			theGame.GetGuiManager().ShowNotification( "WitcherCraft: MINECRAFT mode<br>" + keysLine + "<br>E: inventory | F5: camera | Esc: Minecraft menu (WitcherCraft... = about, options, controls)<br>"
				+ panels + WCraft_KeyName( 1 ) + ": Geralt mode - signs, saving", 7.0 );
		}
		else
		{
			theGame.GetGuiManager().ShowNotification( "WitcherCraft: WITCHER mode (Geralt)<br>Esc: game menu and save, M map, J journal, I inventory, F7 build mode<br>"
				+ WCraft_KeyName( 1 ) + ": back to Minecraft", 6.0 );
		}
	}

	// "G: what | " for a bound key, "" for an unbound one.
	private function KeyHint( which : int, what : string, prefix : string ) : string
	{
		var k : string;

		k = WCraft_KeyName( which );
		if ( k == "none" )
		{
			return "";
		}
		return prefix + k + ": " + what + " | ";
	}

	public function HudFocusTarget( out e : CGameplayEntity ) : bool
	{
		if ( !hud )
		{
			return false;
		}
		return hud.FocusTarget( e );
	}

	public function SetHudHide( on : bool )
	{
		if ( !hud )
		{
			hud = new CWCraftHud in this;
		}
		hud.SetWanted( on );
	}

	private function SetActionsBlocked( on : bool )
	{
		var actions : array< EInputActionBlock >;
		var i : int;

		if ( on == blocked )
		{
			return;
		}
		blocked = on;
		actions.PushBack( EIAB_Movement );
		actions.PushBack( EIAB_Jump );
		actions.PushBack( EIAB_Roll );
		actions.PushBack( EIAB_Dodge );
		actions.PushBack( EIAB_RunAndSprint );
		actions.PushBack( EIAB_Sprint );
		actions.PushBack( EIAB_Explorations );
		actions.PushBack( EIAB_Climb );
		actions.PushBack( EIAB_Slide );
		actions.PushBack( EIAB_Dive );
		// M7: NOT EIAB_CallHorse / EIAB_MountVehicle: X calls Roach and G mounts her; riding
		// suspends Minecraft mode (SuspendReason "vehicle") and Steve resumes where Geralt dismounts.
		actions.PushBack( EIAB_Signs );
		actions.PushBack( EIAB_DrawWeapon );
		actions.PushBack( EIAB_Fists );
		actions.PushBack( EIAB_SwordAttack );
		actions.PushBack( EIAB_LightAttacks );
		actions.PushBack( EIAB_HeavyAttacks );
		actions.PushBack( EIAB_SpecialAttackLight );
		actions.PushBack( EIAB_SpecialAttackHeavy );
		actions.PushBack( EIAB_Parry );
		actions.PushBack( EIAB_Counter );
		actions.PushBack( EIAB_ThrowBomb );
		actions.PushBack( EIAB_Crossbow );
		actions.PushBack( EIAB_UsableItem );
		actions.PushBack( EIAB_QuickSlots );
		actions.PushBack( EIAB_RadialMenu );
		// M7: NOT EIAB_ExplorationFocus: Witcher senses (V) work as Steve.
		// NOT EIAB_Interactions / InteractionAction / InteractionContainers: G hands W3 its own
		// interaction key; the real E is Minecraft's and never reaches W3 here.
		actions.PushBack( EIAB_HardLock );
		actions.PushBack( EIAB_CameraLock );
		for ( i = 0; i < actions.Size(); i += 1 )
		{
			if ( on )
			{
				thePlayer.BlockAction( actions[ i ], 'WitcherCraftMC' );
			}
			else
			{
				thePlayer.UnblockAction( actions[ i ], 'WitcherCraftMC' );
			}
		}
	}

	// The manager's 30 Hz step asks this: after a suspension, put Minecraft where Geralt is.
	public function ConsumeTeleportRequest() : bool
	{
		var r : bool;

		r = wantTeleport;
		wantTeleport = false;
		return r;
	}

	public function IsEngaged() : bool
	{
		return engaged;
	}

	public function SetUseGameCamera( on : bool )
	{
		if ( engaged )
		{
			Disengage( "camera switch" );
		}
		useGameCamera = on;
	}

	public function Status() : string
	{
		var s : string;

		s = "engaged " + engaged + " Geralt " + groundKind + " hidden " + hidden + " invisible " + invisible + " blocked " + blocked + " pitchSign " + FloatToString( pitchSign ) + " gameCamera " + useGameCamera + " frames " + frames;
		if ( cam )
		{
			s += " topmost " + ( theCamera.GetTopmostCamera() == cam );
		}
		return s;
	}
}

// The per-frame loop lives in its own state machine (the manager's loop runs at 30 Hz).
statemachine class CWCraftControlLoop
{
	public var ctl : CWCraftControl;

	public function Start( c : CWCraftControl )
	{
		ctl = c;
		if ( GetCurrentStateName() == 'WCraftControlRunning' )
		{
			GotoState( 'WCraftControlIdle' );
		}
		GotoState( 'WCraftControlRunning' );
	}
}

state WCraftControlIdle in CWCraftControlLoop
{
}

state WCraftControlRunning in CWCraftControlLoop
{
	event OnEnterState( prevStateName : name )
	{
		Run();
	}

	entry function Run()
	{
		while ( true )
		{
			if ( parent.ctl )
			{
				parent.ctl.Frame();
			}
			SleepOneFrame();
		}
	}
}

function WCraft_Control() : CWCraftControl
{
	var mgr : CWCraftManager;

	mgr = WCraft_Manager();
	if ( mgr )
	{
		return mgr.GetControl();
	}
	return NULL;
}

// F8 does the same natively.
exec function wcmode()
{
	WCraft_SetControlMode( !WCraft_ControlMode() );
}

exec function wcmode2( on : bool )
{
	WCraft_SetControlMode( on );
}

// 1 = drive the main game camera instead of the photo-mode camera (fallback if the photo-mode
// camera doesn't follow in your setup); 0 = photo-mode camera (default).
exec function wccam( useGame : int )
{
	var c : CWCraftControl;

	c = WCraft_Control();
	if ( c )
	{
		c.SetUseGameCamera( useGame == 1 );
	}
}

// FOV calibration: 0 vertical (default), 1 horizontal at the real aspect, 2 horizontal at 4:3.
// Pick the one where Minecraft blocks stay glued to the Witcher ground while you turn, then put
// CameraFovMode=<n> under [WitcherCraft] in bin\x64_dx12\witchercraft_native.ini to keep it.
exec function wcfov( mode : int )
{
	var msg : string;

	msg = WCraft_CameraOption( mode, -1 );
	LogChannel( 'WitcherCraft', msg );
	theGame.GetGuiManager().ShowNotification( "WitcherCraft " + msg, 4.0 );
}

// Camera source: 1 = the pose of the overlay frame being drawn (locks the world layer to W3,
// default), 0 = SkyCraft's own 20 Hz tick interpolation. Ini: CameraPose=<n>.
exec function wccamsrc( usePose : int )
{
	var msg : string;

	msg = WCraft_CameraOption( -1, usePose );
	LogChannel( 'WitcherCraft', msg );
	theGame.GetGuiManager().ShowNotification( "WitcherCraft " + msg, 4.0 );
}

// Visual sync status: frame rates, frame generation, the latency in use, how often the exact
// Minecraft frame was drawn. Also written to witchercraft_native.log every 10 s.
exec function wcsync()
{
	var msg : string;

	msg = WCraft_SyncOption( -2, -1 );
	LogChannel( 'WitcherCraft', msg );
	WCraft_ControlNote( "wcsync: " + msg );
	theGame.GetGuiManager().ShowNotification( "WitcherCraft " + msg, 10.0 );
}

// Latency calibration: -1 = measured automatically (default), 0..8 = fixed game frames. Turn
// left and right quickly next to a placed block; keep the value where the block stays glued.
// Ini: OverlayLatencyFrames=<n>.
exec function wclat( frames : int )
{
	var msg : string;

	msg = WCraft_SyncOption( frames, -1 );
	LogChannel( 'WitcherCraft', msg );
	WCraft_ControlNote( "wclat: " + msg );
	theGame.GetGuiManager().ShowNotification( "WitcherCraft " + msg, 6.0 );
}

// 1 = draw the Minecraft frame the Witcher image was rendered with (default), 0 = the newest one
// (the old behaviour, for comparison). Ini: OverlayFrameMatch=<n>.
exec function wcmatch( on : int )
{
	var msg : string;

	msg = WCraft_SyncOption( -2, on );
	LogChannel( 'WitcherCraft', msg );
	theGame.GetGuiManager().ShowNotification( "WitcherCraft " + msg, 6.0 );
}

exec function wcctl()
{
	var c : CWCraftControl;
	var msg : string;

	msg = "mode " + WCraft_ControlMode();
	c = WCraft_Control();
	if ( c )
	{
		msg += "<br>" + c.Status();
	}
	msg += "<br>" + WCraft_GetMcStatus();
	LogChannel( 'WitcherCraft', msg );
	WCraft_ControlNote( "wcctl: " + msg );
	theGame.GetGuiManager().ShowNotification( msg, 8.0 );
}

// Hidden Geralt: 1 = SetHideInGame only (M3 behaviour), 2 = also SetVisibility(false) (no shadow, default).
exec function wchide( mode : int )
{
	var c : CWCraftControl;

	c = WCraft_Control();
	if ( c )
	{
		c.SetHideMode( mode );
	}
}

// M6a: why The Witcher won't save right now; releases a stale exploration lock if Geralt stands.
exec function wcsave()
{
	var c : CWCraftControl;
	var msg : string;

	c = WCraft_Control();
	if ( !c )
	{
		return;
	}
	msg = "save: " + c.SaveDiagnostics() + "<br>" + c.TryUnlockSave();
	LogChannel( 'WitcherCraft', msg );
	WCraft_ControlNote( "wcsave: " + msg );
	theGame.GetGuiManager().ShowNotification( msg, 8.0 );
}
