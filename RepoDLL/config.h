#pragma once

namespace config {
constexpr const char* kAssemblyName = "Assembly-CSharp";

constexpr const char* kGameDirectorNamespace = "";
constexpr const char* kGameDirectorClass = "GameDirector";
constexpr const char* kGameDirectorInstanceField = "instance";
constexpr const char* kGameDirectorPlayerListField = "PlayerList";

constexpr const char* kSemiFuncNamespace = "";
constexpr const char* kSemiFuncClass = "SemiFunc";
constexpr const char* kSemiFuncLocalMethod = "PlayerAvatarLocal";

constexpr const char* kPlayerAvatarNamespace = "";
constexpr const char* kPlayerAvatarClass = "PlayerAvatar";
constexpr const char* kPlayerAvatarIsLocalField = "isLocal";
constexpr const char* kPlayerAvatarInstanceField = "instance";
constexpr const char* kPlayerAvatarTransformField = "playerTransform";
constexpr const char* kPlayerAvatarHealthField = "playerHealth";
constexpr const char* kPlayerAvatarEnergyField = "playerEnergy";  // fallback, not used for stamina
constexpr const char* kPlayerAvatarSteamIdField = "steamID";
constexpr const char* kPlayerAvatarPhysGrabberField = "physGrabber";

constexpr const char* kTransformGetPositionMethod = "get_position";
constexpr const char* kTransformSetPositionMethod = "set_position";

constexpr const char* kPlayerHealthNamespace = "";
constexpr const char* kPlayerHealthClass = "PlayerHealth";
constexpr const char* kPlayerHealthValueField = "health";
constexpr const char* kPlayerHealthMaxField = "maxHealth";
constexpr const char* kPlayerHealthInvincibleSetMethod = "InvincibleSet";

// Stamina actually lives on PlayerController
constexpr const char* kPlayerControllerNamespace = "";
constexpr const char* kPlayerControllerClass = "PlayerController";
constexpr const char* kPlayerControllerInstanceField = "instance";
constexpr const char* kPlayerControllerEnergyCurrentField = "EnergyCurrent";
constexpr const char* kPlayerControllerEnergyStartField = "EnergyStart";
constexpr const char* kPlayerControllerJumpExtraField = "JumpExtra";
constexpr const char* kPlayerControllerOverrideSpeedMultiplierField = "overrideSpeedMultiplier";
constexpr const char* kPlayerControllerOverrideSpeedTimerField = "overrideSpeedTimer";
constexpr const char* kPlayerControllerJumpForceField = "JumpForce";

// PhysGrabber
constexpr const char* kPhysGrabberClass = "PhysGrabber";
constexpr const char* kPhysGrabberGrabRangeField = "grabRange";
constexpr const char* kPhysGrabberGrabStrengthField = "grabStrength";

// Handcart
constexpr const char* kPhysGrabCartClass = "PhysGrabCart";
constexpr const char* kPhysGrabCartHaulCurrentField = "haulCurrent";
constexpr const char* kPhysGrabCartSetHaulTextMethod = "SetHaulText";

// Extraction point
constexpr const char* kExtractionPointClass = "ExtractionPoint";
constexpr const char* kExtractionPointHaulGoalField = "haulGoal";
constexpr const char* kExtractionPointHaulCurrentField = "haulCurrent";

// Currency UI refresh
constexpr const char* kCurrencyUIClass = "CurrencyUI";
constexpr const char* kCurrencyUIFetchMethod = "FetchCurrency";
constexpr const char* kPlayerControllerOverrideSpeedMethod = "OverrideSpeed";
constexpr const char* kPlayerControllerOverrideJumpCooldownMethod = "OverrideJumpCooldown";

// Camera / matrices
constexpr const char* kUnityCameraNamespace = "UnityEngine";
constexpr const char* kUnityCameraClass = "Camera";
constexpr const char* kUnityCameraMainMethod = "get_main";
constexpr const char* kUnityCameraProjectionMatrixMethod = "get_projectionMatrix";
constexpr const char* kUnityCameraWorldToCameraMatrixMethod = "get_worldToCameraMatrix";
constexpr const char* kUnityCameraGetTransformMethod = "get_transform";

constexpr const char* kTransformLocalToWorldMatrixMethod = "get_localToWorldMatrix";
constexpr const char* kTransformWorldToLocalMatrixMethod = "get_worldToLocalMatrix";

// PlayerLocalCamera (if needed later)
constexpr const char* kPlayerLocalCameraNamespace = "";
constexpr const char* kPlayerLocalCameraClass = "PlayerLocalCamera";
constexpr const char* kPlayerLocalCameraInstanceField = "instance";
constexpr const char* kPlayerLocalCameraClientPositionField = "clientPosition";
constexpr const char* kPlayerLocalCameraClientRotationField = "clientRotation";

// Item system
constexpr const char* kItemManagerNamespace = "";
constexpr const char* kItemManagerClass = "ItemManager";
constexpr const char* kItemManagerInstanceField = "instance";
constexpr const char* kItemManagerGetAllItemsMethod = "GetAllItemVolumesInScene";
constexpr const char* kPhysGrabObjectNamespace = "";
constexpr const char* kPhysGrabObjectClass = "PhysGrabObject";
constexpr bool kFindObjectsInactive = true;

constexpr const char* kValuableObjectNamespace = "";
constexpr const char* kValuableObjectClass = "ValuableObject";

// ItemTracker rendering
constexpr const char* kItemTrackerNamespace = "";
constexpr const char* kItemTrackerClass = "ItemTracker";
constexpr const char* kItemTrackerDisplayField = "display";
constexpr const char* kItemTrackerDisplayColorOverrideMethod = "DisplayColorOverride";

// Currency / upgrades
constexpr const char* kSemiFuncStatSetRunCurrencyMethod = "StatSetRunCurrency";
constexpr const char* kSemiFuncStatGetRunCurrencyMethod = "StatGetRunCurrency";

// StatsManager
constexpr const char* kStatsManagerNamespace = "";
constexpr const char* kStatsManagerClass = "StatsManager";
constexpr const char* kStatsManagerInstanceField = "instance";
constexpr const char* kStatsManagerRunStatsField = "runStats";

constexpr const char* kPunManagerNamespace = "";
constexpr const char* kPunManagerClass = "PunManager";
constexpr const char* kPunManagerUpgradeExtraJumpMethod = "UpgradePlayerExtraJump";
constexpr const char* kPunManagerUpgradeGrabStrengthMethod = "UpgradePlayerGrabStrength";
constexpr const char* kPunManagerUpgradeThrowStrengthMethod = "UpgradePlayerThrowStrength";
constexpr const char* kPunManagerSetRunStatSetMethod = "SetRunStatSet";
}  // namespace config
