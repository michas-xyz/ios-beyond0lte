#!/bin/bash
# check-device.sh — Vérifie l'état du téléphone avant toute opération de flash
# Cibles supportées : Galaxy S10e (SM-G970F), S10 (SM-G973F), S10+ (SM-G975F), en TWRP

set -euo pipefail

ADB="${ADB:-$HOME/Developer/android/platform-tools/adb}${B2L_SERIAL:+ -s $B2L_SERIAL}"
adb="$ADB"

echo "=== Vérification du téléphone (check-device.sh) ==="
echo ""

# 1. État TWRP
STATE=$($ADB get-state 2>/dev/null </dev/null || echo "unknown")
echo "-> État adb : $STATE"
if [ "$STATE" != "recovery" ]; then
  echo "ERREUR : le téléphone n'est pas en TWRP/recovery. État actuel : $STATE"
  echo "   -> Redémarre en recovery (Vol- + Power, puis Vol+ + Power)"
  exit 1
fi
echo "   OK : téléphone en TWRP"

# 2. Root
DEVICE_UID=$($ADB shell id -u 2>/dev/null </dev/null | tr -d '\r' || echo "?")
echo "-> DEVICE_UID sur le téléphone : $DEVICE_UID"
if [ "$DEVICE_UID" != "0" ]; then
  echo "ERREUR : le téléphone n'est pas root (uid=$DEVICE_UID)"
  exit 1
fi
echo "   OK : root (DEVICE_UID=0)"

# 3. Modèle (ro.boot.em.model est le seul fiable sur ROM custom)
MODEL=$($ADB shell getprop ro.boot.em.model 2>/dev/null </dev/null | tr -d '\r' || echo "?")
echo "-> ro.boot.em.model : $MODEL"

TARGET_BOARD="${B2L_BOARD:-}"
if [ -n "$TARGET_BOARD" ]; then
  case "$TARGET_BOARD" in
    beyond0lte)
      EXPECTED_MODEL="SM-G970F"
      USERDATA_BLK="sda34"
      USERDATA_SIZE="${B2L_USERDATA_SIZE:-115761721344}"
      ;;
    beyond1lte)
      EXPECTED_MODEL="SM-G973F"
      USERDATA_BLK="sda34"
      USERDATA_SIZE="${B2L_USERDATA_SIZE:-499641200640}"
      ;;
    beyond2lte)
      EXPECTED_MODEL="SM-G975F"
      USERDATA_BLK="sda31"
      USERDATA_SIZE="${B2L_USERDATA_SIZE:-499641200640}"
      ;;
    *)
      echo "ERREUR : B2L_BOARD inconnu : $TARGET_BOARD"
      exit 1
      ;;
  esac
  if [ "$MODEL" != "$EXPECTED_MODEL" ]; then
    echo "ERREUR : modèle inattendu pour $TARGET_BOARD ($MODEL != $EXPECTED_MODEL). ro.product.device est falsifiable sur ROM custom."
    exit 1
  fi
else
  # Résolution dynamique selon le modèle rapporté par le bootloader
  case "$MODEL" in
    SM-G970F)
      TARGET_BOARD="beyond0lte"
      EXPECTED_MODEL="SM-G970F"
      USERDATA_BLK="sda34"
      USERDATA_SIZE="${B2L_USERDATA_SIZE:-115761721344}"
      ;;
    SM-G973F)
      TARGET_BOARD="beyond1lte"
      EXPECTED_MODEL="SM-G973F"
      USERDATA_BLK="sda34"
      USERDATA_SIZE="${B2L_USERDATA_SIZE:-499641200640}"
      ;;
    SM-G975F)
      TARGET_BOARD="beyond2lte"
      EXPECTED_MODEL="SM-G975F"
      USERDATA_BLK="sda31"
      USERDATA_SIZE="${B2L_USERDATA_SIZE:-499641200640}"
      ;;
    *)
      echo "ERREUR : modèle inattendu ($MODEL). Attendu: SM-G970F (S10e), SM-G973F (S10) ou SM-G975F (S10+)."
      exit 1
      ;;
  esac
fi
echo "   OK : modèle $EXPECTED_MODEL ($TARGET_BOARD) confirmé"

# 4. Partitions cibles et leurs blocs
echo ""
echo "=== Partitions cibles ==="
for entry in "boot:sda14:57671680" "system:sda25:7340032000" "userdata:$USERDATA_BLK:$USERDATA_SIZE"; do
  name="${entry%%:*}"
  rest="${entry#*:}"
  blk="${rest%%:*}"
  size="${rest##*:}"
  dev=$($ADB shell readlink -f /dev/block/by-name/$name 2>/dev/null </dev/null | tr -d '\r' || echo "?")
  actual=$($ADB shell blockdev --getsize64 $dev 2>/dev/null </dev/null | tr -d '\r' || echo "?")
  mounted=$($ADB shell "grep -q '^$dev ' /proc/mounts && echo MONTÉ || echo libre" </dev/null 2>/dev/null | tr -d '\r')
  echo "  $name -> $dev (attendu $blk) | taille: $actual (attendu $size) | $mounted"
  if [ "$dev" != "/dev/block/$blk" ] && [ "$dev" != "$blk" ]; then
    echo "   ERREUR : $name ne pointe pas vers $blk (trouvé: $dev)"
    exit 1
  fi
  if [ "$actual" != "$size" ]; then
    # Vérification souple si la taille est supérieure au minimum requis de 32 Gio pour userdata
    if [ "$name" = "userdata" ] && [ "$actual" -ge 34359738368 ] 2>/dev/null; then
      echo "   NOTE : userdata taille ($actual) diffère de $size mais >= 32 Gio requis pour rootfs"
    else
      echo "   ERREUR : taille de $name incorrecte ($actual != $size)"
      exit 1
    fi
  fi
done

echo ""
echo "=== TOUTES LES VÉRIFICATIONS SONT OK ==="
echo "Le téléphone ($TARGET_BOARD / $EXPECTED_MODEL) est prêt pour une opération de flash (avec backup d'abord)."
