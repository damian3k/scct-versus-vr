#pragma once

struct Config {
    bool  enabled = true;
    float worldScale = 100.0f;        // Unreal units per meter
    bool  ignoreGamePitch = true;     // keep the horizon level; head tracking supplies pitch/roll
    int   worldUpAxis = 2;            // 0=X 1=Y 2=Z (Unreal world is Z-up)
    bool  fixShaderMatrices = true;   // correct WVP matrices uploaded as vertex shader constants
    bool  fixProjectedLights = true;  // correct camera-space texgen (flashlight, shadow projectors)
    float shaderMatrixTolerance = 0.02f;
    bool  requireMainRenderTarget = true; // only treat backbuffer-sized targets as the main view
    int   recenterKey = 0x24;         // VK_HOME
    float menuQuadDistance = 3.0f;    // meters
    float menuQuadWidth = 3.2f;       // meters
    // HUD / 2D drawing on its own layer, aligned with the game camera's field of view
    bool  hudLayer = true;
    float hudDistance = 2.5f;         // meters; markers look single when this matches the target's distance
    float hudScale = 0.55f;           // panel size vs the game's field of view (1.0 = markers line up)
    bool  hudFollowAim = true;        // aim mode: HUD follows the game camera (aim) instead of the body
    int   hudToggleKey = 0x2D;        // VK_INSERT: switch between the two HUD modes in game
    float hudFollowScale = 0.45f;      // panel size in the follow-the-weapon mode (game FOV = 1.0)

    // First-person weapon drawn at the right controller (aim mode)
    bool  weaponInHand = true;
    float weaponRadius = 120.0f;      // Unreal units from the camera that count as "the weapon"
    float weaponOffsetX = 10.0f;      // grip point in game camera space (UU): right, up, forward
    float weaponOffsetY = -15.0f;
    float weaponOffsetZ = 20.0f;
    float weaponPitch = 0.0f;         // degrees

    // Controllers
    int   aimMode = 2;                // 0 off, 1 always, 2 auto (only while a first-person weapon is drawn)
    int   minSceneDraws = 10;         // 3D draws needed for a frame to count as stereo gameplay
    bool  laser = false;               // laser beam from the right controller while aiming
    float laserLength = 15.0f;        // meters
    float laserWidth = 0.004f;        // meters
    bool  menuLaser = false;           // menus: the controller ray positions the mouse cursor
    int   aimToggleKey = 0x23;        // VK_END
    float aimConvergence = 10.0f;     // meters along the controller ray the game camera aims at
    float aimGain = 0.5f;             // fraction of the aim error corrected per update
    float mouseCountsPerDegree = 0.0f; // 0 = learn automatically
    float mousePitchScale = 1.0f;
    float turnSpeed = 120.0f;         // degrees/s smooth turn
    float snapTurn = 0.0f;            // degrees per flick, 0 = smooth
    float stickMouseSpeed = 4500.0f;  // mouse counts/s from the right stick in game (spy / aim mode off)
    float menuMouseSpeed = 1500.0f;   // mouse counts/s from the right stick in menus

    // Merc zoom (binocular / sniper mode): head brought to the weapon presses ScopeKey, and while
    // the game camera is zoomed its own (flat) image is shown as a screen in front of the eyes.
    bool  scopeGesture = true;
    float scopeGestureDistance = 0.22f; // meters between the eyes and the right controller
    bool  scopeView = true;
    float scopeMaxFov = 70.0f;        // game horizontal FOV (degrees) below which the game counts as zoomed
    float scopeDistance = 1.0f;       // meters
    float scopeSize = 0.8f;           // screen size vs the game's normal field of view
    float scopeMouseScale = 1.0f;     // zoomed: mouse motion per degree of controller motion (x learned counts/deg)

    // Spy (third person): the right controller's rotation steers the camera / aims (0 = right stick)
    bool  spyAim = true;
    bool  spyStableCamera = true;     // render from a camera that does not orbit when the controller turns
    // Spy first-person view (both stick clicks): eye position relative to the top of the spy's head
    // (UU, right/up/forward; measured automatically when the binoculars are used)
    float spyEyeRight = 0.0f;
    float spyEyeUp = -8.0f;
    float spyEyeForward = 16.0f;
    float spyFpvHideRadius = 60.0f;   // draws whose origin is this close to the spy are its own model
    bool  spyFpvShowBody = true;      // keep drawing the body (hands, arms); only small gear is hidden

    bool  d3d12DebugLayer = false;
    int   statsIntervalSec = 10;
};

extern Config g_cfg;

void LoadConfig();
