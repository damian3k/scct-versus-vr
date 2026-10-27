#include "config.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>

Config g_cfg;

static const wchar_t* IniPath()
{
    static wchar_t path[MAX_PATH];
    if (!path[0]) swprintf_s(path, L"%sSCCT_VR.ini", ModuleDir());
    return path;
}

static int ReadInt(const wchar_t* key, int def)
{
    return (int)GetPrivateProfileIntW(L"VR", key, def, IniPath());
}

static float ReadFloat(const wchar_t* key, float def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(L"VR", key, L"", buf, 64, IniPath());
    if (!buf[0]) return def;
    return (float)_wtof(buf);
}

void LoadConfig()
{
    Config d;
    g_cfg.enabled                 = ReadInt(L"Enabled", d.enabled) != 0;
    g_cfg.backend                 = ReadInt(L"Backend", d.backend);
    g_cfg.xrApiControllerRoll     = ReadFloat(L"XrApiControllerRoll", d.xrApiControllerRoll);
    g_cfg.xrApiStereo             = ReadInt(L"XrApiStereo", d.xrApiStereo) != 0;
    g_cfg.worldScale              = ReadFloat(L"WorldScale", d.worldScale);
    g_cfg.ignoreGamePitch         = ReadInt(L"IgnoreGamePitch", d.ignoreGamePitch) != 0;
    g_cfg.worldUpAxis             = ReadInt(L"WorldUpAxis", d.worldUpAxis);
    g_cfg.fixShaderMatrices       = ReadInt(L"FixShaderMatrices", d.fixShaderMatrices) != 0;
    g_cfg.shaderMatrixTolerance   = ReadFloat(L"ShaderMatrixTolerance", d.shaderMatrixTolerance);
    g_cfg.fixProjectedLights      = ReadInt(L"FixProjectedLights", d.fixProjectedLights) != 0;
    g_cfg.requireMainRenderTarget = ReadInt(L"RequireMainRenderTarget", d.requireMainRenderTarget) != 0;
    g_cfg.recenterKey             = ReadInt(L"RecenterKey", d.recenterKey);
    g_cfg.menuQuadDistance        = ReadFloat(L"MenuQuadDistance", d.menuQuadDistance);
    g_cfg.menuQuadWidth           = ReadFloat(L"MenuQuadWidth", d.menuQuadWidth);
    g_cfg.hudLayer                = ReadInt(L"HudLayer", d.hudLayer) != 0;
    g_cfg.hudDistance             = ReadFloat(L"HudDistance", d.hudDistance);
    g_cfg.hudScale                = ReadFloat(L"HudScale", d.hudScale);
    g_cfg.hudFollowAim            = ReadInt(L"HudFollowAim", d.hudFollowAim) != 0;
    g_cfg.hudToggleKey            = ReadInt(L"HudToggleKey", d.hudToggleKey);
    g_cfg.hudFollowScale          = ReadFloat(L"HudFollowScale", d.hudFollowScale);
    g_cfg.weaponInHand            = ReadInt(L"WeaponInHand", d.weaponInHand) != 0;
    g_cfg.weaponRadius            = ReadFloat(L"WeaponRadius", d.weaponRadius);
    g_cfg.weaponOffsetX           = ReadFloat(L"WeaponOffsetX", d.weaponOffsetX);
    g_cfg.weaponOffsetY           = ReadFloat(L"WeaponOffsetY", d.weaponOffsetY);
    g_cfg.weaponOffsetZ           = ReadFloat(L"WeaponOffsetZ", d.weaponOffsetZ);
    g_cfg.weaponPitch             = ReadFloat(L"WeaponPitch", d.weaponPitch);
    g_cfg.aimMode                 = ReadInt(L"AimMode", d.aimMode);
    g_cfg.minSceneDraws           = ReadInt(L"MinSceneDraws", d.minSceneDraws);
    g_cfg.laser                   = ReadInt(L"Laser", d.laser) != 0;
    g_cfg.laserLength             = ReadFloat(L"LaserLength", d.laserLength);
    g_cfg.laserWidth              = ReadFloat(L"LaserWidth", d.laserWidth);
    g_cfg.menuLaser               = ReadInt(L"MenuLaser", d.menuLaser) != 0;
    g_cfg.aimToggleKey            = ReadInt(L"AimToggleKey", d.aimToggleKey);
    g_cfg.aimConvergence          = ReadFloat(L"AimConvergence", d.aimConvergence);
    g_cfg.aimGain                 = ReadFloat(L"AimGain", d.aimGain);
    g_cfg.mouseCountsPerDegree    = ReadFloat(L"MouseCountsPerDegree", d.mouseCountsPerDegree);
    g_cfg.mousePitchScale         = ReadFloat(L"MousePitchScale", d.mousePitchScale);
    g_cfg.turnSpeed               = ReadFloat(L"TurnSpeed", d.turnSpeed);
    g_cfg.snapTurn                = ReadFloat(L"SnapTurn", d.snapTurn);
    g_cfg.stickMouseSpeed         = ReadFloat(L"StickMouseSpeed", d.stickMouseSpeed);
    g_cfg.menuMouseSpeed          = ReadFloat(L"MenuMouseSpeed", d.menuMouseSpeed);
    g_cfg.scopeGesture            = ReadInt(L"ScopeGesture", d.scopeGesture) != 0;
    g_cfg.scopeGestureDistance    = ReadFloat(L"ScopeGestureDistance", d.scopeGestureDistance);
    g_cfg.scopeView               = ReadInt(L"ScopeView", d.scopeView) != 0;
    g_cfg.scopeMaxFov             = ReadFloat(L"ScopeMaxFov", d.scopeMaxFov);
    g_cfg.scopeDistance           = ReadFloat(L"ScopeDistance", d.scopeDistance);
    g_cfg.scopeSize               = ReadFloat(L"ScopeSize", d.scopeSize);
    g_cfg.scopeMouseScale         = ReadFloat(L"ScopeMouseScale", d.scopeMouseScale);
    g_cfg.reloadGesture           = ReadInt(L"ReloadGesture", d.reloadGesture) != 0;
    g_cfg.reloadForward           = ReadFloat(L"ReloadForward", d.reloadForward);
    g_cfg.reloadDown              = ReadFloat(L"ReloadDown", d.reloadDown);
    g_cfg.reloadRadius            = ReadFloat(L"ReloadRadius", d.reloadRadius);
    g_cfg.spyAim                  = ReadInt(L"SpyAim", d.spyAim) != 0;
    g_cfg.spyStableCamera         = ReadInt(L"SpyStableCamera", d.spyStableCamera) != 0;
    g_cfg.spyEyeRight             = ReadFloat(L"SpyEyeRight", d.spyEyeRight);
    g_cfg.spyEyeUp                = ReadFloat(L"SpyEyeUp", d.spyEyeUp);
    g_cfg.spyEyeForward           = ReadFloat(L"SpyEyeForward", d.spyEyeForward);
    g_cfg.spyFpvHideRadius        = ReadFloat(L"SpyFpvHideRadius", d.spyFpvHideRadius);
    g_cfg.spyFpvShowBody          = ReadInt(L"SpyFpvShowBody", d.spyFpvShowBody) != 0;
    g_cfg.desktopMirror           = ReadInt(L"DesktopMirror", d.desktopMirror) != 0;
    g_cfg.dumpEverySec            = ReadInt(L"DumpEverySec", d.dumpEverySec);
    g_cfg.d3d12DebugLayer         = ReadInt(L"D3D12DebugLayer", d.d3d12DebugLayer) != 0;
    g_cfg.statsIntervalSec        = ReadInt(L"StatsIntervalSec", d.statsIntervalSec);
    if (g_cfg.worldUpAxis < 0 || g_cfg.worldUpAxis > 2) g_cfg.worldUpAxis = 2;

    Log("Config: Enabled=%d WorldScale=%.1f IgnoreGamePitch=%d WorldUpAxis=%d FixShaderMatrices=%d Tol=%.3f RequireMainRT=%d RecenterKey=0x%X",
        g_cfg.enabled, g_cfg.worldScale, g_cfg.ignoreGamePitch, g_cfg.worldUpAxis, g_cfg.fixShaderMatrices,
        g_cfg.shaderMatrixTolerance, g_cfg.requireMainRenderTarget, g_cfg.recenterKey);
    Log("Config: HudLayer=%d HudDistance=%.2f HudScale=%.2f AimMode=%d AimConvergence=%.1f AimGain=%.2f MouseCountsPerDegree=%.2f TurnSpeed=%.0f SnapTurn=%.0f",
        g_cfg.hudLayer, g_cfg.hudDistance, g_cfg.hudScale, g_cfg.aimMode, g_cfg.aimConvergence, g_cfg.aimGain,
        g_cfg.mouseCountsPerDegree, g_cfg.turnSpeed, g_cfg.snapTurn);
}
