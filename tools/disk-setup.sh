#!/usr/bin/env bash
set -euo pipefail

# === Configuración ===

DISK_IMG="disk.img"

# Directorios de paquetes a inyectar (añade aquí más: pkg-foo, pkg-bar, ...)
PKG_DIRS=("pkg-hello")

# Versión por defecto para los paquetes
PKG_VERSION="0.1.0"

# === Ruta fija a la raíz del proyecto ===
ROOT_DIR="/home/antonio/Documentos/MesaOS-Lite/microkernel"

cd "$ROOT_DIR"

echo "== MesaOS package injector =="
echo "Raíz del proyecto: $ROOT_DIR"
echo "Disco: $DISK_IMG"
echo

# === Comprobar que existe disk.img (ya particionado y formateado por ti / por el OS) ===

if [ ! -f "$DISK_IMG" ]; then
    echo "ERROR: no existe $DISK_IMG."
    echo "Crea el disco primero con:  make format-disk"
    echo "Luego crea la partición 0x77 con fdisk y formatea (si toca) con ./tools/mesafs-format disk.img"
    exit 1
fi

# === Comprobar herramientas necesarias ===

if [ ! -x "$ROOT_DIR/tools/inject-file" ] || \
   [ ! -x "$ROOT_DIR/tools/msa-create" ]; then
    echo "ERROR: faltan herramientas en $ROOT_DIR/tools (inject-file, msa-create)."
    echo "Compílalas primero (por ejemplo: 'make tools' o 'make all')."
    exit 1
fi

echo "=== Creando e inyectando paquetes .msa (sin tocar particiones ni formato) ==="

for PKG_DIR in "${PKG_DIRS[@]}"; do
    if [ ! -d "${PKG_DIR}" ]; then
        echo "  [SKIP] No existe el directorio de paquete ${PKG_DIR}"
        continue
    fi

    NAME="${PKG_DIR#pkg-}"              # pkg-hello -> hello
    MSA="${NAME}-${PKG_VERSION}.msa"

    echo "  -> Creando paquete ${NAME} v${PKG_VERSION} desde ${PKG_DIR}..."
    "$ROOT_DIR/tools/msa-create" \
        -n "${NAME}" \
        -v "${PKG_VERSION}" \
        -a "MesaOS User" \
        -d "${NAME} package for MesaOS" \
        "${PKG_DIR}" \
        "${MSA}"

    echo "  -> Inyectando ${MSA} en ${DISK_IMG} como /pkgs/${MSA}..."
    "$ROOT_DIR/tools/inject-file" "${DISK_IMG}" "${MSA}" "/pkgs/${MSA}"
done

echo
echo "== Inyección completada =="
echo "Ahora arranca con:  make run"