{ self }:
{ config, lib, pkgs, ... }:
let
  inherit (lib)
    concatLists
    escapeShellArgs
    mkEnableOption
    mkIf
    mkOption
    optionals
    types
    ;

  packagesForSystem = lib.attrByPath [ pkgs.system ] (self.packages or { }) { };

  defaultPackage =
    packagesForSystem.scribe-tap
    or packagesForSystem.default
    or (throw "scribe-tap package not available for system ${pkgs.system}");

  cfg = config.services.scribeTap;

  floatToString = value:
    if builtins.isFloat value || builtins.isInt value then lib.strings.floatToString value else value;

  toPathString = value: toString value;

  resolvedDataDir = toPathString cfg.dataDir;
  resolvedLogDir = toPathString (cfg.logDir or "${resolvedDataDir}/logs");
  resolvedSnapshotDir = toPathString (cfg.snapshotDir or "${resolvedDataDir}/snapshots");

  baseArgs =
    concatLists [
      [ "--data-dir" resolvedDataDir ]
      [ "--log-dir" resolvedLogDir ]
      [ "--snapshot-dir" resolvedSnapshotDir ]
      [ "--snapshot-interval" (floatToString cfg.snapshotInterval) ]
      [ "--context-refresh" (floatToString cfg.contextRefresh) ]
      [ "--clipboard" cfg.clipboardMode ]
      [ "--context" cfg.contextMode ]
      [ "--log-mode" cfg.logMode ]
      [ "--translate" cfg.translateMode ]
      (optionals (cfg.xkbLayout != null) [
        "--xkb-layout"
        cfg.xkbLayout
      ])
      (optionals (cfg.xkbVariant != null) [
        "--xkb-variant"
        cfg.xkbVariant
      ])
      (optionals (cfg.hyprSignaturePath != null) [
        "--hypr-signature"
        cfg.hyprSignaturePath
      ])
      (optionals (cfg.hyprUser != null) [
        "--hypr-user"
        cfg.hyprUser
      ])
      (optionals (cfg.hyprctlCommand != null) [
        "--hyprctl"
        cfg.hyprctlCommand
      ])
      cfg.extraArgs
    ];

  commandList = [ "${cfg.package}/bin/scribe-tap" ] ++ baseArgs;
  commandString = escapeShellArgs commandList;

  tmpfilesRules =
    map
      (dir: "d ${dir} ${cfg.directoryMode} ${cfg.directoryUser} ${cfg.directoryGroup} - -")
      (lib.unique [ resolvedDataDir resolvedLogDir resolvedSnapshotDir ]);
in
{
  options.services.scribeTap = {
    enable = mkEnableOption "scribe-tap interception helper";

    package = mkOption {
      type = types.package;
      default = defaultPackage;
      description = "Package providing the scribe-tap executable.";
    };

    installSystemPackage = mkOption {
      type = types.bool;
      default = true;
      description = "Add scribe-tap to environment.systemPackages when enabled.";
    };

    dataDir = mkOption {
      type = types.path;
      default = "/var/lib/scribe-tap";
      description = "Root directory for runtime state and derived log directories.";
    };

    logDir = mkOption {
      type = types.nullOr types.path;
      default = null;
      description = "Explicit log directory. Defaults to <dataDir>/logs.";
    };

    snapshotDir = mkOption {
      type = types.nullOr types.path;
      default = null;
      description = "Explicit snapshot directory. Defaults to <dataDir>/snapshots.";
    };

    snapshotInterval = mkOption {
      type = types.number;
      default = 5.0;
      description = "Interval (seconds) between log snapshots.";
    };

    contextRefresh = mkOption {
      type = types.number;
      default = 0.4;
      description = "Polling interval (seconds) for context refresh.";
    };

    clipboardMode = mkOption {
      type = types.enum [ "auto" "off" ];
      default = "auto";
      description = "Clipboard handling mode passed to --clipboard.";
    };

    contextMode = mkOption {
      type = types.enum [ "hyprland" "none" ];
      default = "hyprland";
      description = "Context provider passed to --context.";
    };

    logMode = mkOption {
      type = types.enum [ "events" "snapshots" "both" ];
      default = "both";
      description = "Logging mode passed to --log-mode.";
    };

    translateMode = mkOption {
      type = types.enum [ "xkb" "raw" ];
      default = "xkb";
      description = "Translation mode passed to --translate.";
    };

    xkbLayout = mkOption {
      type = types.nullOr types.str;
      default = null;
      description = "Keyboard layout forwarded via --xkb-layout.";
    };

    xkbVariant = mkOption {
      type = types.nullOr types.str;
      default = null;
      description = "Keyboard layout variant forwarded via --xkb-variant.";
    };

    hyprctlCommand = mkOption {
      type = types.str;
      default = "hyprctl";
      description = "Command used for Hyprland inspection (--hyprctl).";
    };

    hyprSignaturePath = mkOption {
      type = types.nullOr types.str;
      default = null;
      description = "Signature file path passed via --hypr-signature when provided.";
    };

    hyprUser = mkOption {
      type = types.nullOr types.str;
      default = null;
      description = "Hyprland user supplied via --hypr-user.";
    };

    extraArgs = mkOption {
      type = types.listOf types.str;
      default = [ ];
      description = "Additional arguments appended to the generated command.";
    };

    ensureDirectories = mkOption {
      type = types.bool;
      default = true;
      description = "Create data/log/snapshot directories via systemd-tmpfiles.";
    };

    directoryMode = mkOption {
      type = types.str;
      default = "0750";
      description = "Filesystem mode applied to managed directories.";
    };

    directoryUser = mkOption {
      type = types.str;
      default = "root";
      description = "Owner applied to managed directories.";
    };

    directoryGroup = mkOption {
      type = types.str;
      default = "root";
      description = "Group applied to managed directories.";
    };

    command = mkOption {
      type = types.listOf types.str;
      default = [ ];
      description = "Resolved scribe-tap invocation expressed as a list.";
    };

    commandString = mkOption {
      type = types.str;
      default = "";
      description = "Resolved scribe-tap invocation rendered for shell pipelines.";
    };

    resolvedPaths = mkOption {
      type = types.attrsOf types.str;
      default = { };
      description = "Resolved directory paths exposed for downstream consumers.";
    };
  };

  config = mkIf cfg.enable {
    environment.systemPackages = optionals cfg.installSystemPackage [ cfg.package ];

    services.scribeTap.command = commandList;
    services.scribeTap.commandString = commandString;
    services.scribeTap.resolvedPaths = {
      dataDir = resolvedDataDir;
      logDir = resolvedLogDir;
      snapshotDir = resolvedSnapshotDir;
    };

    systemd.tmpfiles.rules = lib.mkIf cfg.ensureDirectories (lib.mkAfter tmpfilesRules);
  };
}
