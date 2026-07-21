# Shared QuackMail "core" plumbing.
#
# All QuackMail extensions are configured in a single CMake invocation (via
# extension_config.cmake), and the `build_static_extension` variants are linked
# together into DuckDB's unittest binary. To avoid multiply-defined symbols
# there, core is compiled EXACTLY ONCE into a static library (guarded below) and
# linked into every extension, rather than compiled into each.
#
# Usage from an extension CMakeLists.txt:
#   include(${CMAKE_CURRENT_LIST_DIR}/../core/quackmail_core.cmake)
#   include_directories(${QUACKMAIL_CORE_INCLUDE})
#   ... build_static_extension / build_loadable_extension ...
#   quackmail_link(${EXTENSION_NAME})
#   quackmail_link(${LOADABLE_EXTENSION_NAME})

get_filename_component(QUACKMAIL_CORE_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
set(QUACKMAIL_CORE_INCLUDE ${QUACKMAIL_CORE_DIR}/include)

# TLS + password hashing use the system OpenSSL (libssl-dev). DuckDB's bundled
# mbedTLS is crypto-only (no TLS handshake), so it cannot serve here.
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

if(NOT TARGET quackmail_core)
    add_library(quackmail_core STATIC
        ${QUACKMAIL_CORE_DIR}/src/mail_store.cpp
        ${QUACKMAIL_CORE_DIR}/src/net.cpp
        ${QUACKMAIL_CORE_DIR}/src/server_controller.cpp
        ${QUACKMAIL_CORE_DIR}/src/server_controls.cpp
        ${QUACKMAIL_CORE_DIR}/src/tls.cpp
        ${QUACKMAIL_CORE_DIR}/src/mime.cpp
        ${QUACKMAIL_CORE_DIR}/src/mime_codec.cpp
        ${QUACKMAIL_CORE_DIR}/src/mime_entity.cpp
        ${QUACKMAIL_CORE_DIR}/src/rfc5322.cpp
        ${QUACKMAIL_CORE_DIR}/src/auth.cpp
        ${QUACKMAIL_CORE_DIR}/src/sieve.cpp
        ${QUACKMAIL_CORE_DIR}/src/util.cpp
    )
    # Linked into loadable .so extensions, so it must be position independent.
    set_target_properties(quackmail_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(quackmail_core PUBLIC ${QUACKMAIL_CORE_INCLUDE})
    target_link_libraries(quackmail_core PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

    # The per-extension static libs are added to DuckDB's export set and link
    # quackmail_core, so quackmail_core must live in the same export set.
    install(TARGETS quackmail_core
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
endif()

# Link core (and its transitive OpenSSL/pthread) into a QuackMail extension target.
function(quackmail_link TARGET)
    target_link_libraries(${TARGET} quackmail_core)
endfunction()
