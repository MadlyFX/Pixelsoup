if(NOT DEFINED SERVER_EXE OR NOT EXISTS "${SERVER_EXE}")
    message(FATAL_ERROR "SERVER_EXE must name the built media-server executable")
endif()
if(NOT DEFINED TEST_ROOT OR TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "TEST_ROOT is required")
endif()
if(NOT DEFINED TEST_BINARY_ROOT OR TEST_BINARY_ROOT STREQUAL "")
    message(FATAL_ERROR "TEST_BINARY_ROOT is required")
endif()

set(test_root_path "${TEST_ROOT}")
set(test_binary_root_path "${TEST_BINARY_ROOT}")
cmake_path(
    ABSOLUTE_PATH test_root_path
    NORMALIZE
    OUTPUT_VARIABLE test_root_absolute
)
cmake_path(
    ABSOLUTE_PATH test_binary_root_path
    NORMALIZE
    OUTPUT_VARIABLE test_binary_root_absolute
)
cmake_path(
    IS_PREFIX test_binary_root_absolute
    "${test_root_absolute}"
    NORMALIZE
    test_root_is_scoped
)
cmake_path(GET test_root_absolute FILENAME test_root_name)
if(NOT test_root_is_scoped OR NOT test_root_name STREQUAL "bootstrap-smoke")
    message(FATAL_ERROR
        "refusing to clean unscoped bootstrap test directory: ${test_root_absolute}")
endif()
set(TEST_ROOT "${test_root_absolute}")

# This directory is generated under the current CMake binary tree and contains
# only smoke-test artifacts.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

execute_process(
    COMMAND "${SERVER_EXE}" --validate-startup
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE default_result
    OUTPUT_VARIABLE default_output
    ERROR_VARIABLE default_error
)
if(NOT default_result EQUAL 0)
    message(FATAL_ERROR
        "node-only default startup failed: ${default_result}: ${default_error}")
endif()
if(EXISTS "${TEST_ROOT}/media-server.wal")
    message(FATAL_ERROR "node-only startup created the Director WAL")
endif()
if(EXISTS "${TEST_ROOT}/media-server.wal.release")
    message(FATAL_ERROR "node-only startup created the Director release ledger")
endif()
if(NOT default_output MATCHES "node-session=disabled")
    message(FATAL_ERROR
        "standalone node startup did not report a disabled connector: ${default_output}")
endif()
if(default_output MATCHES "boundary-driver=")
    message(FATAL_ERROR
        "standalone node startup reported a connector boundary driver: ${default_output}")
endif()

set(show_id "00000000000000010000000000000002")
set(node_id "00000000000000030000000000000004")
set(node_incarnation "00000000000000050000000000000006")
set(node_endpoint "127.0.0.1:9")

# Validation constructs and checks the outbound connector configuration but
# must neither start the asynchronous supervisor nor require a reachable peer.
execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=output
        "--director-connect=${node_endpoint}"
        "--show-id=${show_id}"
        "--node-id=${node_id}"
        "--node-incarnation=${node_incarnation}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE node_connector_result
    OUTPUT_VARIABLE node_connector_output
    ERROR_VARIABLE node_connector_error
)
if(NOT node_connector_result EQUAL 0)
    message(FATAL_ERROR
        "node connector validation failed without a Director: "
        "${node_connector_result}: ${node_connector_error}")
endif()
if(NOT node_connector_output MATCHES "node-session=configured")
    message(FATAL_ERROR
        "node connector validation did not report configured state: "
        "${node_connector_output}")
endif()
if(NOT node_connector_output MATCHES "director-connect=127\\.0\\.0\\.1:9")
    message(FATAL_ERROR
        "node connector validation did not report its endpoint: "
        "${node_connector_output}")
endif()
if(NOT node_connector_output MATCHES "boundary-driver=unwired")
    message(FATAL_ERROR
        "node connector validation hid the unwired boundary driver: "
        "${node_connector_output}")
endif()
if(EXISTS "${TEST_ROOT}/media-server.wal"
    OR EXISTS "${TEST_ROOT}/media-server.wal.release")
    message(FATAL_ERROR
        "node connector validation created Director persistence files")
endif()

execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=output
        "--director-connect=${node_endpoint}"
        "--show-id=${show_id}"
        "--node-id=${node_id}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE partial_connector_result
    OUTPUT_VARIABLE partial_connector_output
    ERROR_VARIABLE partial_connector_error
)
if(partial_connector_result EQUAL 0)
    message(FATAL_ERROR
        "node startup accepted a partial connector identity")
endif()

execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=output
        "--show-id=${show_id}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE unused_identity_result
    OUTPUT_VARIABLE unused_identity_output
    ERROR_VARIABLE unused_identity_error
)
if(unused_identity_result EQUAL 0)
    message(FATAL_ERROR
        "node startup accepted an identity without --director-connect")
endif()

set(invalid_node_endpoints
    "127.0.0.1:0"
    "0.0.0.0:9000"
    "239.1.2.3:9000"
    "255.255.255.255:9000"
    "localhost:9000"
)
foreach(invalid_node_endpoint IN LISTS invalid_node_endpoints)
    execute_process(
        COMMAND
            "${SERVER_EXE}"
            --validate-startup
            --role=output
            "--director-connect=${invalid_node_endpoint}"
            "--show-id=${show_id}"
            "--node-id=${node_id}"
            "--node-incarnation=${node_incarnation}"
        WORKING_DIRECTORY "${TEST_ROOT}"
        RESULT_VARIABLE invalid_endpoint_result
        OUTPUT_VARIABLE invalid_endpoint_output
        ERROR_VARIABLE invalid_endpoint_error
    )
    if(invalid_endpoint_result EQUAL 0)
        message(FATAL_ERROR
            "node startup accepted non-unicast/non-numeric endpoint: "
            "${invalid_node_endpoint}")
    endif()
endforeach()

set(mixed_role_wal "${TEST_ROOT}/mixed-role-must-not-open.wal")
execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=director,output
        "--wal=${mixed_role_wal}"
        "--director-connect=${node_endpoint}"
        "--show-id=${show_id}"
        "--node-id=${node_id}"
        "--node-incarnation=${node_incarnation}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE mixed_role_result
    OUTPUT_VARIABLE mixed_role_output
    ERROR_VARIABLE mixed_role_error
)
if(mixed_role_result EQUAL 0)
    message(FATAL_ERROR
        "Director role accepted outbound node connector ownership")
endif()
if(EXISTS "${mixed_role_wal}" OR EXISTS "${mixed_role_wal}.release")
    message(FATAL_ERROR
        "invalid mixed-role connector configuration opened Director persistence")
endif()

set(node_wal "${TEST_ROOT}/node-must-not-open.wal")
execute_process(
    COMMAND "${SERVER_EXE}" --validate-startup --role=output "--wal=${node_wal}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE node_result
    OUTPUT_VARIABLE node_output
    ERROR_VARIABLE node_error
)
if(node_result EQUAL 0)
    message(FATAL_ERROR "node-only startup accepted explicit Director WAL ownership")
endif()
if(EXISTS "${node_wal}")
    message(FATAL_ERROR "node-only startup opened the explicit Director WAL")
endif()

set(director_wal "${TEST_ROOT}/director.wal")
set(director_release_ledger "${director_wal}.release")
execute_process(
    COMMAND "${SERVER_EXE}" --validate-startup --role=director,output "--wal=${director_wal}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE director_result
    OUTPUT_VARIABLE director_output
    ERROR_VARIABLE director_error
)
if(NOT director_result EQUAL 0)
    message(FATAL_ERROR
        "Director startup failed: ${director_result}: ${director_error}")
endif()
if(NOT EXISTS "${director_wal}")
    message(FATAL_ERROR "Director startup did not create its WAL")
endif()
if(NOT EXISTS "${director_release_ledger}")
    message(FATAL_ERROR "Director startup did not create its release ledger")
endif()
if(NOT director_output MATCHES "director-session-hub=initialized")
    message(FATAL_ERROR
        "Director startup did not report session hub initialization: ${director_output}")
endif()
if(NOT director_output MATCHES "director-listener=disabled")
    message(FATAL_ERROR
        "Director startup incorrectly implied that a listener exists: ${director_output}")
endif()
file(SIZE "${director_wal}" director_wal_size)
if(NOT director_wal_size EQUAL 64)
    message(FATAL_ERROR
        "new genesis WAL has unexpected size: ${director_wal_size}")
endif()
file(SIZE "${director_release_ledger}" director_release_ledger_size)
if(NOT director_release_ledger_size EQUAL 64)
    message(FATAL_ERROR
        "new genesis release ledger has unexpected size: "
        "${director_release_ledger_size}")
endif()

execute_process(
    COMMAND "${SERVER_EXE}" --validate-startup --role=director,output "--wal=${director_wal}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE restart_result
    OUTPUT_VARIABLE restart_output
    ERROR_VARIABLE restart_error
)
if(NOT restart_result EQUAL 0)
    message(FATAL_ERROR
        "Director restart failed: ${restart_result}: ${restart_error}")
endif()

execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=director,output
        "--wal=${director_wal}"
        --director-listen=127.0.0.1:0
        --show-id=00000000000000010000000000000002
        --director-epoch=00000000000000030000000000000004
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE listener_result
    OUTPUT_VARIABLE listener_output
    ERROR_VARIABLE listener_error
)
if(NOT listener_result EQUAL 0)
    message(FATAL_ERROR
        "explicit Director listener validation failed: "
        "${listener_result}: ${listener_error}")
endif()
if(NOT listener_output MATCHES "director-listener=127\\.0\\.0\\.1:[0-9]+")
    message(FATAL_ERROR
        "Director did not report its bound listener: ${listener_output}")
endif()
if(NOT listener_output MATCHES "commit-barrier=durable-permit-gated")
    message(FATAL_ERROR
        "Director did not report the durable commit gate: ${listener_output}")
endif()
if(NOT listener_output MATCHES "release-frontier=0")
    message(FATAL_ERROR
        "Director did not report the genesis release frontier: ${listener_output}")
endif()

execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=director
        "--wal=${director_wal}"
        --director-listen=127.0.0.1:0
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE missing_identity_result
    OUTPUT_VARIABLE missing_identity_output
    ERROR_VARIABLE missing_identity_error
)
if(missing_identity_result EQUAL 0)
    message(FATAL_ERROR
        "Director listener accepted missing show/epoch identity")
endif()

# Without the explicit validation flag, bootstrap transitions into the service
# lifecycle and must remain alive rather than reporting readiness then exiting.
execute_process(
    COMMAND "${SERVER_EXE}" --role=output
    WORKING_DIRECTORY "${TEST_ROOT}"
    TIMEOUT 0.5
    RESULT_VARIABLE service_result
    OUTPUT_VARIABLE service_output
    ERROR_VARIABLE service_error
)
if(NOT service_result MATCHES "[Tt]imeout")
    message(FATAL_ERROR
        "normal service startup did not remain alive: ${service_result}: ${service_error}")
endif()

# With a complete connector quartet, normal startup launches supervision and
# remains alive while connection refusal is handled as retryable backoff. It
# deliberately does not invent a show-clock checkpoint/frame boundary.
execute_process(
    COMMAND
        "${SERVER_EXE}"
        --role=output
        "--director-connect=${node_endpoint}"
        "--show-id=${show_id}"
        "--node-id=${node_id}"
        "--node-incarnation=${node_incarnation}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    # Long enough for a Debug build to load and report through the disk log's
    # console forwarder (about 0.45 s warm); the node must still be running.
    TIMEOUT 2
    RESULT_VARIABLE connector_service_result
    OUTPUT_VARIABLE connector_service_output
    ERROR_VARIABLE connector_service_error
)
if(NOT connector_service_result MATCHES "[Tt]imeout")
    message(FATAL_ERROR
        "node connector supervision did not remain alive: "
        "${connector_service_result}: ${connector_service_error}")
endif()
if(NOT connector_service_output MATCHES "node-session=supervising")
    message(FATAL_ERROR
        "normal node startup did not report connector supervision: "
        "${connector_service_output}")
endif()
if(NOT connector_service_output MATCHES "boundary-driver=unwired")
    message(FATAL_ERROR
        "normal node startup hid the unwired boundary driver: "
        "${connector_service_output}")
endif()
if(EXISTS "${TEST_ROOT}/media-server.wal"
    OR EXISTS "${TEST_ROOT}/media-server.wal.release")
    message(FATAL_ERROR
        "normal node connector startup created Director persistence files")
endif()

# Playback wiring. Validation must construct and check the show clock without
# starting a transport, dialing, or sealing anything.
execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=output
        "--director-connect=${node_endpoint}"
        "--show-id=${show_id}"
        "--node-id=${node_id}"
        "--node-incarnation=${node_incarnation}"
        --playback
        --show-rate=60000/1001
        --show-range=100:200
        --node-index=3
        --surface=320x180
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE node_playback_result
    OUTPUT_VARIABLE node_playback_output
    ERROR_VARIABLE node_playback_error
)
if(NOT node_playback_result EQUAL 0)
    message(FATAL_ERROR
        "node playback validation failed: "
        "${node_playback_result}: ${node_playback_error}")
endif()
if(NOT node_playback_output MATCHES "boundary-driver=show-clock")
    message(FATAL_ERROR
        "node playback did not report a wired boundary driver: "
        "${node_playback_output}")
endif()
if(NOT node_playback_output MATCHES "show-rate=60000/1001")
    message(FATAL_ERROR
        "node playback did not report its exact rational rate: "
        "${node_playback_output}")
endif()
if(NOT node_playback_output MATCHES "show-range=100:200")
    message(FATAL_ERROR
        "node playback did not report its show range: ${node_playback_output}")
endif()

# The unqualified launch cadence and range are the new-project defaults.
execute_process(
    COMMAND "${SERVER_EXE}" --validate-startup --role=output
        --director-connect=auto --show-id=auto --node-index=auto
        "--node-id=${node_id}" "--node-incarnation=${node_incarnation}"
        --playback
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE auto_discovery_result
    OUTPUT_VARIABLE auto_discovery_output
    ERROR_VARIABLE auto_discovery_error
    TIMEOUT 10
)
if(NOT auto_discovery_result EQUAL 0)
    message(FATAL_ERROR "automatic editor discovery startup validation failed: ${auto_discovery_error}")
endif()

execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=output
        "--director-connect=${node_endpoint}"
        "--show-id=${show_id}"
        "--node-id=${node_id}"
        "--node-incarnation=${node_incarnation}"
        --playback
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE default_playback_result
    OUTPUT_VARIABLE default_playback_output
    ERROR_VARIABLE default_playback_error
)
if(NOT default_playback_result EQUAL 0
    OR NOT default_playback_output MATCHES "show-rate=24/1"
    OR NOT default_playback_output MATCHES "show-range=0:7199")
    message(FATAL_ERROR
        "default playback cadence/range changed: "
        "${default_playback_result}: ${default_playback_output} ${default_playback_error}")
endif()

# Playback on a node is meaningless without a Director to approve frames, and
# on a Director without the listener that releases them.
execute_process(
    COMMAND "${SERVER_EXE}" --validate-startup --role=output --playback
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE detached_playback_result
    OUTPUT_VARIABLE detached_playback_output
    ERROR_VARIABLE detached_playback_error
)
if(detached_playback_result EQUAL 0)
    message(FATAL_ERROR
        "node playback was accepted without a Director connection")
endif()

execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=director
        "--wal=${director_wal}"
        --playback
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE unlistened_playback_result
    OUTPUT_VARIABLE unlistened_playback_output
    ERROR_VARIABLE unlistened_playback_error
)
if(unlistened_playback_result EQUAL 0)
    message(FATAL_ERROR
        "Director playback was accepted without a commit-barrier listener")
endif()

execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=output
        "--director-connect=${node_endpoint}"
        "--show-id=${show_id}"
        "--node-id=${node_id}"
        "--node-incarnation=${node_incarnation}"
        --run-frames=10
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE stray_transport_result
    OUTPUT_VARIABLE stray_transport_output
    ERROR_VARIABLE stray_transport_error
)
if(stray_transport_result EQUAL 0)
    message(FATAL_ERROR
        "transport options were accepted without --playback")
endif()

set(invalid_playback_options
    "--show-rate=0"
    "--show-rate=50/0"
    "--show-rate=2000000"
    "--show-rate=fifty"
    "--show-range=200:100"
    "--show-range=100"
    "--surface=0x180"
    "--surface=320"
    "--surface=99999x180"
    "--node-index=99999"
    "--window-at=-2147483649,0"
    "--window-at=0,2147483648"
    "--window-at=10"
    "--window-at=10,20,30"
)
foreach(invalid_playback_option IN LISTS invalid_playback_options)
    execute_process(
        COMMAND
            "${SERVER_EXE}"
            --validate-startup
            --role=output
            "--director-connect=${node_endpoint}"
            "--show-id=${show_id}"
            "--node-id=${node_id}"
            "--node-incarnation=${node_incarnation}"
            --playback
            "${invalid_playback_option}"
        WORKING_DIRECTORY "${TEST_ROOT}"
        RESULT_VARIABLE invalid_playback_result
        OUTPUT_VARIABLE invalid_playback_output
        ERROR_VARIABLE invalid_playback_error
    )
    if(invalid_playback_result EQUAL 0)
        message(FATAL_ERROR
            "playback accepted an invalid option: ${invalid_playback_option}")
    endif()
endforeach()

# Hardware Mapping uses signed desktop coordinates because displays can sit to
# the left of or above the primary monitor. The CLI fallback must accept the
# same representation. Validation does not open the requested window.
execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=output
        "--director-connect=${node_endpoint}"
        "--show-id=${show_id}"
        "--node-id=${node_id}"
        "--node-incarnation=${node_incarnation}"
        --playback
        --window
        --window-at=-1920,-120
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE signed_window_result
    OUTPUT_VARIABLE signed_window_output
    ERROR_VARIABLE signed_window_error
)
if(NOT signed_window_result EQUAL 0)
    message(FATAL_ERROR
        "signed window position was rejected: "
        "${signed_window_result}: ${signed_window_error}")
endif()
if(NOT signed_window_output MATCHES "window-at=-1920,-120")
    message(FATAL_ERROR
        "signed window position did not round-trip through validation: "
        "${signed_window_output}")
endif()

execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --role=director,output
        "--wal=${director_wal}"
        --director-listen=127.0.0.1:0
        "--show-id=${show_id}"
        --director-epoch=00000000000000030000000000000004
        --playback
        --show-rate=25
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE director_playback_result
    OUTPUT_VARIABLE director_playback_output
    ERROR_VARIABLE director_playback_error
)
if(NOT director_playback_result EQUAL 0)
    message(FATAL_ERROR
        "Director playback validation failed: "
        "${director_playback_result}: ${director_playback_error}")
endif()
if(NOT director_playback_output MATCHES "show-clock=director-transport")
    message(FATAL_ERROR
        "Director playback did not report its transport ownership: "
        "${director_playback_output}")
endif()

# Standalone render-process lifecycle agent. Validation reads the local password
# and fixes the child command without binding a port or launching the child.
set(lifecycle_secret "${TEST_ROOT}/lifecycle.secret")
file(WRITE "${lifecycle_secret}"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --lifecycle-listen=127.0.0.1:47840
        --lifecycle-node-index=3
        "--lifecycle-secret-file=${lifecycle_secret}"
        "--lifecycle-child=${SERVER_EXE}"
        --lifecycle-child-arg=--role=output
        --lifecycle-autostart
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE lifecycle_result
    OUTPUT_VARIABLE lifecycle_output
    ERROR_VARIABLE lifecycle_error
)
if(NOT lifecycle_result EQUAL 0)
    message(FATAL_ERROR
        "lifecycle agent validation failed: ${lifecycle_result}: "
        "${lifecycle_error}")
endif()
execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --lifecycle-autostart
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE orphan_autostart_result
    OUTPUT_VARIABLE orphan_autostart_output
    ERROR_VARIABLE orphan_autostart_error
)
if(orphan_autostart_result EQUAL 0)
    message(FATAL_ERROR
        "lifecycle autostart was accepted without a lifecycle listener")
endif()
if(NOT lifecycle_output MATCHES
    "lifecycle agent validated; node-index=3")
    message(FATAL_ERROR
        "lifecycle validation hid its explicit node identity: "
        "${lifecycle_output}")
endif()

# The password is optional: an agent with none validates (and runs
# unsecured), as does one given a short typed password. A password file that
# cannot be read as one - here a directory - is still refused.
foreach(password_option IN ITEMS "" "--lifecycle-password=hunter2")
    execute_process(
        COMMAND
            "${SERVER_EXE}"
            --validate-startup
            --lifecycle-listen=127.0.0.1:47840
            --lifecycle-node-index=3
            ${password_option}
            "--lifecycle-child=${SERVER_EXE}"
        WORKING_DIRECTORY "${TEST_ROOT}"
        RESULT_VARIABLE password_lifecycle_result
        OUTPUT_VARIABLE password_lifecycle_output
        ERROR_VARIABLE password_lifecycle_error
    )
    if(NOT password_lifecycle_result EQUAL 0)
        message(FATAL_ERROR
            "lifecycle agent refused '${password_option}': "
            "${password_lifecycle_error}")
    endif()
endforeach()
execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --lifecycle-listen=127.0.0.1:47840
        --lifecycle-node-index=3
        "--lifecycle-secret-file=${TEST_ROOT}"
        "--lifecycle-child=${SERVER_EXE}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE unreadable_lifecycle_result
    OUTPUT_VARIABLE unreadable_lifecycle_output
    ERROR_VARIABLE unreadable_lifecycle_error
)
if(unreadable_lifecycle_result EQUAL 0)
    message(FATAL_ERROR
        "lifecycle agent accepted a password file it could not read")
endif()

# Discovery answers on a UDP port the agent and the editor share; 0 turns it
# off. A port that cannot exist is refused, and the option means nothing to a
# process that is neither an agent nor an editor.
execute_process(
    COMMAND
        "${SERVER_EXE}"
        --validate-startup
        --lifecycle-listen=127.0.0.1:47840
        --lifecycle-node-index=3
        --lifecycle-discovery-port=0
        "--lifecycle-child=${SERVER_EXE}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE discovery_off_result
    OUTPUT_VARIABLE discovery_off_output
    ERROR_VARIABLE discovery_off_error
)
if(NOT discovery_off_result EQUAL 0)
    message(FATAL_ERROR
        "lifecycle agent refused discovery off: ${discovery_off_error}")
endif()
foreach(discovery_case IN ITEMS
        "--lifecycle-listen=127.0.0.1:47840;--lifecycle-node-index=3;--lifecycle-discovery-port=65536;--lifecycle-child=${SERVER_EXE}"
        "--role=output;--lifecycle-discovery-port=47839")
    execute_process(
        COMMAND "${SERVER_EXE}" --validate-startup ${discovery_case}
        WORKING_DIRECTORY "${TEST_ROOT}"
        RESULT_VARIABLE discovery_bad_result
        OUTPUT_VARIABLE discovery_bad_output
        ERROR_VARIABLE discovery_bad_error
    )
    if(discovery_bad_result EQUAL 0)
        message(FATAL_ERROR
            "accepted a discovery port it should refuse: ${discovery_case}")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
