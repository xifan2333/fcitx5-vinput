# cmake/Dependencies.cmake Discovers system shared libraries (Tier 1 dependencies) and sets up
# imported targets.

find_package(PkgConfig REQUIRED)
find_package(Gettext REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

find_package(LibArchive REQUIRED)
find_package(OpenSSL REQUIRED)

# Import systemd / sd-bus via pkg-config
pkg_check_modules(SDBUS REQUIRED IMPORTED_TARGET libsystemd)

# Import PipeWire 0.3 via pkg-config
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
