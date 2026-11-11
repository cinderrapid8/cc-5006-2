# Source this file to make the user-local toolchain visible.
# shellcheck disable=SC2148
_SROOT=/workspace/.sysroot/root
export PATH="$_SROOT/usr/bin:$_SROOT/usr/lib/qt6/libexec:$PATH"
export LD_LIBRARY_PATH="$_SROOT/lib/aarch64-linux-gnu:$_SROOT/usr/lib/aarch64-linux-gnu:$_SROOT/usr/lib/aarch64-linux-gnu/pulseaudio:$_SROOT/usr/lib/aarch64-linux-gnu/blas:$_SROOT/usr/lib/aarch64-linux-gnu/lapack:$_SROOT/usr/lib:${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="$_SROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$_SROOT/usr/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$_SROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$_SROOT/usr/share/pkgconfig"
export QT_PLUGIN_PATH="$_SROOT/usr/lib/aarch64-linux-gnu/qt6/plugins"
unset _SROOT
