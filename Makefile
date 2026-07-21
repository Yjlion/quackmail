PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Name of the "primary" extension (the umbrella). All QuackMail extensions are
# declared in extension_config.cmake and built together.
EXT_NAME=quackmail
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# QuackMail uses the system OpenSSL (libssl-dev) for TLS + password hashing, so
# no vcpkg toolchain is required. See core/quackmail_core.cmake.

# Include the reusable extension build Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
