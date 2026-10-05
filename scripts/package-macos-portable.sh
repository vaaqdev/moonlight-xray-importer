#!/bin/bash
# Run on macOS to preserve bundle symlinks, executable modes and signatures.
set -euo pipefail

app="${1:-build/build-Release/app/Moonlight.app}"
output="${2:-build/portable}"
name="Moonlight-6.2.0-macOS-Portable"
mkdir -p "$output"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$name"
ditto "$app" "$stage/$name/Moonlight.app"

cat > "$stage/$name/LEEME.txt" <<'README'
Moonlight Importer portable para macOS 13 o posterior
Compatible con Intel y Apple Silicon. Xray y dependencias incluidos.

1. Descomprime TODO el ZIP con Utilidad de Compresion de macOS.
2. Abre Moonlight.app desde esta carpeta, Descargas o un USB.
   No necesitas copiarlo a Aplicaciones ni ejecutar un instalador.
3. Importa tu perfil JSON en Moonlight y reinicia la app cuando lo pida.

La app tiene firma ad-hoc y no esta notarizada por Apple. Si macOS bloquea
su apertura, utiliza la opcion de abrir en Privacidad y seguridad, cuando
este disponible. En un Mac administrado de biblioteca, las restricciones
del equipo pueden impedir ejecutarla; pide ayuda al personal en ese caso.

Esta distribucion es portable para ejecutar, no para almacenar todos los
datos: el perfil, ajustes y diagnosticos pueden guardarse en la cuenta
del usuario del Mac. Antes de salir de una computadora compartida, elimina
tu perfil importado y los datos personales que hayas guardado.

No se incluyen perfiles, credenciales ni configuraciones personales.
Codigo fuente GPLv3: https://github.com/vaaqdev/moonlight-xray-importer
README

zip="$output/$name.zip"
ditto -c -k --sequesterRsrc --keepParent "$stage/$name" "$zip"
mkdir -p "$stage/verify"
ditto -x -k "$zip" "$stage/verify"
python3 scripts/verify-macos-bundle.py "$stage/verify/$name/Moonlight.app"
shasum -a 256 "$zip"
