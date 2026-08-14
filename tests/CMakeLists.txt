include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)

# ---------------------------------------------------------------------------
# naTE_tests — UI-independent unit tests (no wx)
# ---------------------------------------------------------------------------
add_executable(naTE_tests
    main.cpp
    test_config.cpp
    test_env_utils.cpp
    test_url_scanner.cpp
    test_word_selector.cpp
    test_dialog_placement.cpp
    test_pane_geometry.cpp
    test_remote_path.cpp
    test_dir_model.cpp
    test_transfer_queue.cpp
    test_explorer_controller.cpp
    test_link_resolver.cpp
    test_sftp_task_queue.cpp
    test_remote_deleter.cpp
    test_local_filesystem.cpp
    test_edit_workspace.cpp
    test_remote_edit.cpp
    ${CMAKE_SOURCE_DIR}/src/config/Config.cpp
    ${CMAKE_SOURCE_DIR}/src/config/ColorScheme.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/EnvUtils.cpp
    ${CMAKE_SOURCE_DIR}/src/layout/UrlScanner.cpp
    ${CMAKE_SOURCE_DIR}/src/layout/WordSelector.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/FileMode.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/RemotePath.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/DirModel.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/TransferQueue.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/ExplorerController.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/RemoteDeleter.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/LinkResolver.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/EditWorkspace.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/SaveAnnouncePolicy.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/RemoteEditSession.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/RemoteEditManager.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/LocalFileSystem.cpp
)
target_link_libraries(naTE_tests PRIVATE Catch2::Catch2)
target_include_directories(naTE_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# naTE_core_tests — headless integration tests: SessionManager + mock observer
#                   No wx dependency; exercises the full core stack.
# ---------------------------------------------------------------------------
set(CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/config/Config.cpp
    ${CMAKE_SOURCE_DIR}/src/config/ColorScheme.cpp
    ${CMAKE_SOURCE_DIR}/src/document/Document.cpp
    ${CMAKE_SOURCE_DIR}/src/input/InputRouter.cpp
    ${CMAKE_SOURCE_DIR}/src/parser/Parser.cpp
    ${CMAKE_SOURCE_DIR}/src/session/Connection.cpp
    ${CMAKE_SOURCE_DIR}/src/session/InputEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/session/Session.cpp
    ${CMAKE_SOURCE_DIR}/src/session/SessionManager.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/EnvUtils.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/BastionTunnel.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/SshConfig.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/SshSession.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/SshSftpService.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/SshPortForwardEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/SshPublicKey.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/X11Utils.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/LoopbackTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/PtyTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/SerialTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/SshAuthenticator.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/SshChannelProxy.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/SshTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/FileMode.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/RemotePath.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/DirModel.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/TransferQueue.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/ExplorerController.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/RemoteDeleter.cpp
    ${CMAKE_SOURCE_DIR}/src/fs/LinkResolver.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/LocalFileSystem.cpp
    ${CMAKE_SOURCE_DIR}/src/layout/DocLayout.cpp
    ${CMAKE_SOURCE_DIR}/src/layout/UrlScanner.cpp
    ${CMAKE_SOURCE_DIR}/src/layout/WordSelector.cpp
    ${CMAKE_SOURCE_DIR}/src/db/JsonScrollbackRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/db/ScrollbackWriter.cpp
    ${CMAKE_SOURCE_DIR}/src/db/ScrollbackPurge.cpp
)

add_executable(naTE_core_tests
    test_document.cpp
    test_session_manager.cpp
    test_x11_utils.cpp
    test_cwd.cpp
    test_scrollback_repo.cpp
    test_remote_fs.cpp
    ${CORE_SOURCES}
)
target_link_libraries(naTE_core_tests PRIVATE Catch2::Catch2WithMain ${NATE_LIBSSH2_LINK} nlohmann_json::nlohmann_json)
target_include_directories(naTE_core_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${NATE_LIBSSH2_INCLUDE})

if(UNIX AND NOT APPLE)
    target_link_libraries(naTE_core_tests PRIVATE util)
endif()

# ---------------------------------------------------------------------------
# naTE_restore_tests — headless unit tests for session restore persistence.
#                      No wx or libssh2 dependency.
# ---------------------------------------------------------------------------
add_executable(naTE_restore_tests
    test_session_restore.cpp
    test_named_workspace.cpp
    test_ssh_auth.cpp
    test_port_forward.cpp
    ${CMAKE_SOURCE_DIR}/src/db/JsonSessionRestoreRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/db/JsonNamedWorkspaceRepository.cpp
    ${CMAKE_SOURCE_DIR}/src/session/Connection.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/SshConfig.cpp
    ${CMAKE_SOURCE_DIR}/src/transport/SshPublicKey.cpp
)
target_link_libraries(naTE_restore_tests PRIVATE Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
target_include_directories(naTE_restore_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
include(CTest)
include(Catch)
catch_discover_tests(naTE_tests)
catch_discover_tests(naTE_core_tests)
catch_discover_tests(naTE_restore_tests)
