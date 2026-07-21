# Included by DuckDB's build system. Declares every QuackMail extension that
# should be built and linked into the DuckDB test binary / produced as a
# loadable .duckdb_extension.
#
# Each component of the mail server is its own extension, coordinating through
# shared DuckDB tables (the database is the bus). Common C++ plumbing lives in
# core/ and is compiled into each extension (see core/quackmail_core.cmake).

# Umbrella: schema init, status aggregate, user management (qm_user_add/remove).
duckdb_extension_load(quackmail
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/quackmail
)

# Inbound SMTP MTA (fully implemented).
duckdb_extension_load(quackmail_smtp_in
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/smtp_in
)

# Outbound SMTP relay / submission queue drainer.
duckdb_extension_load(quackmail_smtp_out
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/smtp_out
)

# IMAP4rev1 retrieval.
duckdb_extension_load(quackmail_imap
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/imap
)

# ManageSieve (RFC 5804) script management.
duckdb_extension_load(quackmail_managesieve
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/managesieve
)

# POP3 retrieval.
duckdb_extension_load(quackmail_pop3
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/pop3
)
