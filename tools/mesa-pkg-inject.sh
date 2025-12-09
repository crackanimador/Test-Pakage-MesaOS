#!/bin/sh
set -e

PKG_DIR="$1"    # ej: pkg-hello
DISK_IMG="$2"   # ej: disk.img
VERSION="${3:-1.0.0}"  # versión opcional, por defecto 1.0.0

if [ -z "$PKG_DIR" ] || [ -z "$DISK_IMG" ]; then
    echo "Uso: $0 <pkg-dir> <disk.img> [version]"
    echo "Ej:  $0 pkg-hello disk.img 0.1.0"
    exit 1
fi

NAME=$(basename "$PKG_DIR" | sed 's/^pkg-//')
MSA="${NAME}-${VERSION}.msa"

# 1) Crear el paquete .msa
./tools/msa-create \
    -n "$NAME" \
    -v "$VERSION" \
    -a "MesaOS User" \
    -d "$NAME package for MesaOS" \
    "$PKG_DIR" \
    "$MSA"

# 2) Inyectarlo en la imagen, dentro de /pkgs
./tools/inject-file "$DISK_IMG" "$MSA" "/pkgs/$MSA"

echo "Paquete $NAME-$VERSION inyectado en $DISK_IMG como /pkgs/$MSA"