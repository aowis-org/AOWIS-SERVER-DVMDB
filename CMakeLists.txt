cmake_minimum_required(VERSION 3.22)
project(AOWIS_Server_DVMDB VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(aowis-dvmdb-store STATIC
    src/native_store.cpp
    src/model_record.cpp
    src/access_policy.cpp
    src/object_id.cpp
    src/revision_record.cpp
    src/revision_resolver.cpp
)
target_compile_features(aowis-dvmdb-store PUBLIC cxx_std_20)
target_include_directories(aowis-dvmdb-store PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aowis-dvmdb-store PUBLIC OpenSSL::Crypto)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aowis-dvmdb-store PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

# Project catalogue/lifecycle is a direct, Qt-free C++ API and remains usable
# by the standalone build independently of network transport.
option(AOWIS_DVMDB_VM_DELETION_BARRIERS
    "Enable blocking deletion crash barriers (VM qualification builds ONLY)" OFF)
add_library(aowis-dvmdb-project-service STATIC src/project_service.cpp)
add_library(AOWIS::DvmdbProjectService ALIAS aowis-dvmdb-project-service)
target_compile_features(aowis-dvmdb-project-service PUBLIC cxx_std_20)
if(AOWIS_DVMDB_VM_DELETION_BARRIERS)
    target_compile_definitions(aowis-dvmdb-project-service PRIVATE
        AOWIS_DVMDB_VM_DELETION_BARRIERS=1)
endif()
target_include_directories(aowis-dvmdb-project-service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aowis-dvmdb-project-service PUBLIC aowis-dvmdb-store)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aowis-dvmdb-project-service PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(aowis-dvmdb-access tools/native_access.cpp)
target_link_libraries(aowis-dvmdb-access PRIVATE aowis-dvmdb-store)

add_executable(aowis-dvmdb-sync tools/native_sync.cpp)
target_link_libraries(aowis-dvmdb-sync PRIVATE aowis-dvmdb-store)

add_executable(aowis-dvmdb-verify tools/native_verify.cpp)
target_link_libraries(aowis-dvmdb-verify PRIVATE aowis-dvmdb-store)

add_executable(aowis-dvmdb-compact tools/native_compact.cpp)
target_link_libraries(aowis-dvmdb-compact PRIVATE aowis-dvmdb-store)

add_executable(aowis-dvmdb-index-scaling-benchmark benchmarks/native_index_scaling_benchmark.cpp)
target_link_libraries(aowis-dvmdb-index-scaling-benchmark PRIVATE aowis-dvmdb-store)

add_executable(aowis-dvmdb-benchmark benchmarks/native_benchmark.cpp)
target_link_libraries(aowis-dvmdb-benchmark PRIVATE aowis-dvmdb-store)

add_executable(aowis-dvmdb-merge-history-benchmark benchmarks/native_merge_history_benchmark.cpp)
target_link_libraries(aowis-dvmdb-merge-history-benchmark PRIVATE aowis-dvmdb-store)

add_executable(aowis-dvmdb-replication-scaling-benchmark
    benchmarks/native_replication_scaling_benchmark.cpp)
target_link_libraries(aowis-dvmdb-replication-scaling-benchmark PRIVATE aowis-dvmdb-store)

add_executable(aowis-dvmdb-model-benchmark benchmarks/model_network_benchmark.cpp)
target_link_libraries(aowis-dvmdb-model-benchmark PRIVATE aowis-dvmdb-store)

if(BUILD_TESTING)
    # Frozen pre-release bytes built by a previous native engine, not generated
    # by the executable under test. Reopen the historical pack and import its
    # full and incremental bundles before changing stored format encodings.
    add_executable(aowis-dvmdb-format-compatibility-tests
        tests/native_format_compatibility_tests.cpp)
    target_link_libraries(aowis-dvmdb-format-compatibility-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-format-compatibility-tests
        COMMAND aowis-dvmdb-format-compatibility-tests
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/pack-20261024")
    set_tests_properties(aowis-dvmdb-format-compatibility-tests PROPERTIES
        LABELS "aowis;compatibility" TIMEOUT 90)
    add_executable(aowis-dvmdb-transfer-staging-budget-tests
        tests/native_transfer_staging_budget_tests.cpp)
    target_include_directories(aowis-dvmdb-transfer-staging-budget-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aowis-dvmdb-transfer-staging-budget-tests PRIVATE cxx_std_20)
    target_link_libraries(aowis-dvmdb-transfer-staging-budget-tests PRIVATE Threads::Threads)
    add_test(NAME aowis-dvmdb-transfer-staging-budget-tests
        COMMAND aowis-dvmdb-transfer-staging-budget-tests)
    set_tests_properties(aowis-dvmdb-transfer-staging-budget-tests PROPERTIES LABELS "aowis")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME aowis-dvmdb-model-fingerprint-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/model_codec_fingerprint_tests.py"
            --cmake "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ModelCodecFingerprint.cmake"
            --model "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL")
    set_tests_properties(aowis-dvmdb-model-fingerprint-tests PROPERTIES LABELS "aowis;qualification")
    # Checks that every canonical UUIDv7 HTTP route matches its OpenAPI
    # method/path, even in native-only builds without Qt development headers.
    add_test(NAME aowis-dvmdb-http-uuidv7-route-contract-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_uuidv7_route_contract_tests.py"
            --source "${CMAKE_CURRENT_SOURCE_DIR}/src/dvmdb_http_routes.cpp"
            --openapi "${CMAKE_CURRENT_SOURCE_DIR}/docs/dvmdb-http-api.yaml")
    set_tests_properties(aowis-dvmdb-http-uuidv7-route-contract-tests PROPERTIES
        LABELS "aowis;http;qualification" TIMEOUT 30)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME aowis-dvmdb-build-launch-guard-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_linux_guard_tests.py"
                --script "${CMAKE_CURRENT_SOURCE_DIR}/compile_linux.sh")
        set_tests_properties(aowis-dvmdb-build-launch-guard-tests PROPERTIES
            LABELS "aowis;qualification" TIMEOUT 30)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME aowis-dvmdb-http-resource-monitor-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_http_resources_tests.py")
        set_tests_properties(aowis-dvmdb-http-resource-monitor-tests PROPERTIES
            LABELS "aowis;http-stress;qualification" TIMEOUT 30)
        # Offline analysis of saved resource reports. This does not start a
        # server or assert a universal throughput/memory threshold.
        add_test(NAME aowis-dvmdb-http-resource-analysis-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_resource_run_analysis_tests.py")
        set_tests_properties(aowis-dvmdb-http-resource-analysis-tests PROPERTIES
            LABELS "aowis;http-stress;qualification" TIMEOUT 30)
    endif()
    add_test(NAME aowis-dvmdb-index-scaling-smoke
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark_native_index.py"
            --binary "$<TARGET_FILE:aowis-dvmdb-index-scaling-benchmark>" --smoke)
    set_tests_properties(aowis-dvmdb-index-scaling-smoke PROPERTIES LABELS "aowis")
    add_test(NAME aowis-dvmdb-index-benchmark-build-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/index_benchmark_build_tests.py"
            --script "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark_native_index.py")
    set_tests_properties(aowis-dvmdb-index-benchmark-build-tests PROPERTIES LABELS "aowis")
    # Offline runner tests: no Qt or hydraulic executable is needed here.
    add_test(NAME aowis-dvmdb-hydraulic-workload-driver-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydraulic_workload_driver_tests.py")
    set_tests_properties(aowis-dvmdb-hydraulic-workload-driver-tests PROPERTIES
        LABELS "aowis;qualification" TIMEOUT 30)
    add_test(NAME aowis-dvmdb-hydraulic-workload-analysis-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydraulic_workload_analysis_tests.py")
    set_tests_properties(aowis-dvmdb-hydraulic-workload-analysis-tests PROPERTIES
        LABELS "aowis;qualification" TIMEOUT 30)
    # Packaging must include the working-tree files of every Git submodule,
    # including recursive submodules. The test creates disposable Git repos.
    find_program(AOWIS_TEST_GIT_EXECUTABLE git)
    find_program(AOWIS_TEST_ZIP_EXECUTABLE zip)
    if(AOWIS_TEST_GIT_EXECUTABLE AND AOWIS_TEST_ZIP_EXECUTABLE)
        add_test(NAME aowis-dvmdb-zip-submodule-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/zip_submodule_tests.py"
                --script "${CMAKE_CURRENT_SOURCE_DIR}/tools/zip_dvmdb.sh")
        set_tests_properties(aowis-dvmdb-zip-submodule-tests PROPERTIES LABELS "aowis")
        # The DVMDB checkout accepts only DVMDB patches and creates one canonical source archive.
        add_test(NAME aowis-dvmdb-naming-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/dvmdb_naming_tests.py"
                --apply-script "${CMAKE_CURRENT_SOURCE_DIR}/tools/apply_patch.sh"
                --zip-script "${CMAKE_CURRENT_SOURCE_DIR}/tools/zip_dvmdb.sh"
                --source-root "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(aowis-dvmdb-naming-tests PROPERTIES LABELS "aowis;qualification")
    endif()
    add_test(NAME aowis-dvmdb-replication-benchmark-smoke
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark_replication_scaling.py"
            --binary "$<TARGET_FILE:aowis-dvmdb-replication-scaling-benchmark>" --smoke)
    set_tests_properties(aowis-dvmdb-replication-benchmark-smoke PROPERTIES LABELS "aowis")
    add_test(NAME aowis-dvmdb-replication-benchmark-phases
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/replication_benchmark_phases_tests.py"
            --script "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark_replication_scaling.py"
            --binary "$<TARGET_FILE:aowis-dvmdb-replication-scaling-benchmark>")
    set_tests_properties(aowis-dvmdb-replication-benchmark-phases PROPERTIES LABELS "aowis")
    # Compile the engine a second time exclusively for fault-injection tests.
    # Production targets never contain crash/failure injection hooks.
    add_library(aowis-dvmdb-store-fault-tests STATIC
        src/native_store.cpp
        src/model_record.cpp
        src/access_policy.cpp
        src/object_id.cpp
        src/revision_record.cpp
    src/revision_resolver.cpp
    )
    target_compile_features(aowis-dvmdb-store-fault-tests PUBLIC cxx_std_20)
    target_include_directories(aowis-dvmdb-store-fault-tests PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(aowis-dvmdb-store-fault-tests PUBLIC OpenSSL::Crypto)
    target_compile_definitions(aowis-dvmdb-store-fault-tests PRIVATE AOWIS_DVMDB_FAULT_TEST_BUILD)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(aowis-dvmdb-store-fault-tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
    # Docker qualification setup can be validated offline without Docker/QEMU.
    add_test(NAME aowis-dvmdb-vm-powercut-docker-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_powercut_docker_tests.py"
            --wrapper "${CMAKE_CURRENT_SOURCE_DIR}/qualify_vm_powercut_docker.sh"
            --helper "${CMAKE_CURRENT_SOURCE_DIR}/tools/vm_powercut_docker.py")
    set_tests_properties(aowis-dvmdb-vm-powercut-docker-tests PROPERTIES
        LABELS "aowis;durability" TIMEOUT 30)
    # Offline orchestration regression: a whole-VM kill may discard the worker
    # installed on the *guest OS* disk; recovery must restage it each boot.
    # Mocked QEMU operations never format, boot, or open any real disk.
    add_test(NAME aowis-dvmdb-vm-powercut-runner-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_powercut_runner_tests.py")
    set_tests_properties(aowis-dvmdb-vm-powercut-runner-tests PROPERTIES
        LABELS "aowis;durability" TIMEOUT 30)
    # Offline evidence checker for QEMU, CTest and HTTP-soak qualification
    # reports; cannot itself exercise a VM or a production filesystem.
    add_test(NAME aowis-dvmdb-qualification-evidence-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/qualification_evidence_tests.py")
    set_tests_properties(aowis-dvmdb-qualification-evidence-tests PROPERTIES
        LABELS "aowis;durability" TIMEOUT 30)
    # Standalone production-library worker for optional, externally orchestrated
    # QEMU power-cut qualification. Not registered in default CTest: it needs a
    # separately provisioned guest OS and disposable virtual disk.
    add_executable(aowis-dvmdb-vm-powercut-worker
        tests/native_vm_powercut_worker.cpp)
    target_link_libraries(aowis-dvmdb-vm-powercut-worker PRIVATE
        aowis-dvmdb-store aowis-dvmdb-project-service)
    add_test(NAME aowis-dvmdb-vm-powercut-worker-smoke
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_powercut_worker_tests.py"
            --binary "$<TARGET_FILE:aowis-dvmdb-vm-powercut-worker>")
    set_tests_properties(aowis-dvmdb-vm-powercut-worker-smoke PROPERTIES
        LABELS "aowis;durability" TIMEOUT 90)
    if(AOWIS_DVMDB_VM_DELETION_BARRIERS)
        add_test(NAME aowis-dvmdb-vm-deletion-barrier-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_powercut_worker_tests.py"
                --binary "$<TARGET_FILE:aowis-dvmdb-vm-powercut-worker>" --barriers)
        set_tests_properties(aowis-dvmdb-vm-deletion-barrier-tests PROPERTIES
            LABELS "aowis;durability;deletion" TIMEOUT 180)
    endif()

    # Abrupt SIGKILL of the production store in independent processes,
    # followed by fresh-process verification and continued writes. This is
    # not equivalent to removing power from the block device.
    add_executable(aowis-dvmdb-sigkill-restart-tests tests/native_sigkill_restart_tests.cpp)
    target_link_libraries(aowis-dvmdb-sigkill-restart-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-sigkill-restart-tests
        COMMAND aowis-dvmdb-sigkill-restart-tests)
    set_tests_properties(aowis-dvmdb-sigkill-restart-tests PROPERTIES
        LABELS "aowis;durability" TIMEOUT 120)
    add_executable(aowis-dvmdb-merge-tests tests/native_merge_tests.cpp)
    target_link_libraries(aowis-dvmdb-merge-tests PRIVATE aowis-dvmdb-project-service aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-merge-tests COMMAND aowis-dvmdb-merge-tests)
    set_tests_properties(aowis-dvmdb-merge-tests PROPERTIES LABELS "aowis;identity")
    add_executable(aowis-dvmdb-project-model-tests tests/native_project_model_tests.cpp)
    target_link_libraries(aowis-dvmdb-project-model-tests PRIVATE aowis-dvmdb-project-service)
    add_test(NAME aowis-dvmdb-project-model-tests COMMAND aowis-dvmdb-project-model-tests)
    set_tests_properties(aowis-dvmdb-project-model-tests PROPERTIES LABELS "aowis;identity;durability")
    add_executable(aowis-dvmdb-revision-head-tests tests/native_repository_revision_head_tests.cpp)
    target_link_libraries(aowis-dvmdb-revision-head-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-revision-head-tests COMMAND aowis-dvmdb-revision-head-tests)
    set_tests_properties(aowis-dvmdb-revision-head-tests PROPERTIES LABELS "aowis;identity;durability")
    add_executable(aowis-dvmdb-packed-revision-tests tests/native_repository_revision_tests.cpp)
    target_link_libraries(aowis-dvmdb-packed-revision-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-packed-revision-tests COMMAND aowis-dvmdb-packed-revision-tests)
    set_tests_properties(aowis-dvmdb-packed-revision-tests PROPERTIES LABELS "aowis;identity;durability")
    add_executable(aowis-dvmdb-revision-resolver-tests tests/revision_resolver_tests.cpp)
    target_link_libraries(aowis-dvmdb-revision-resolver-tests PRIVATE aowis-dvmdb-store Threads::Threads)
    add_test(NAME aowis-dvmdb-revision-resolver-tests COMMAND aowis-dvmdb-revision-resolver-tests)
    set_tests_properties(aowis-dvmdb-revision-resolver-tests PROPERTIES LABELS "aowis;identity;durability")
    add_executable(aowis-dvmdb-revision-record-tests tests/revision_record_tests.cpp)
    target_link_libraries(aowis-dvmdb-revision-record-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-revision-record-tests COMMAND aowis-dvmdb-revision-record-tests)
    set_tests_properties(aowis-dvmdb-revision-record-tests PROPERTIES LABELS "aowis;identity")
    add_executable(aowis-dvmdb-revision-bundle-tests tests/native_revision_bundle_tests.cpp)
    target_link_libraries(aowis-dvmdb-revision-bundle-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-revision-bundle-tests COMMAND aowis-dvmdb-revision-bundle-tests)
    set_tests_properties(aowis-dvmdb-revision-bundle-tests PROPERTIES LABELS "aowis;identity;replication")
    add_executable(aowis-dvmdb-revision-bundle-large-tests tests/native_revision_bundle_large_tests.cpp)
    target_link_libraries(aowis-dvmdb-revision-bundle-large-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-revision-bundle-large-tests COMMAND aowis-dvmdb-revision-bundle-large-tests)
    set_tests_properties(aowis-dvmdb-revision-bundle-large-tests PROPERTIES LABELS "aowis;identity;replication;stress")
    add_executable(aowis-dvmdb-revision-graph-tests tests/native_revision_graph_tests.cpp)
    target_link_libraries(aowis-dvmdb-revision-graph-tests PRIVATE aowis-dvmdb-project-service)
    add_test(NAME aowis-dvmdb-revision-graph-tests COMMAND aowis-dvmdb-revision-graph-tests)
    set_tests_properties(aowis-dvmdb-revision-graph-tests PROPERTIES LABELS "aowis;identity")
    add_executable(aowis-dvmdb-branch-id-tests tests/native_branch_identity_tests.cpp)
    target_link_libraries(aowis-dvmdb-branch-id-tests PRIVATE aowis-dvmdb-project-service)
    add_test(NAME aowis-dvmdb-branch-id-tests COMMAND aowis-dvmdb-branch-id-tests)
    set_tests_properties(aowis-dvmdb-branch-id-tests PROPERTIES LABELS "aowis;identity")

    add_executable(aowis-dvmdb-object-id-tests tests/object_id_tests.cpp)
    target_link_libraries(aowis-dvmdb-object-id-tests PRIVATE aowis-dvmdb-project-service Threads::Threads)
    add_test(NAME aowis-dvmdb-object-id-tests COMMAND aowis-dvmdb-object-id-tests)
    set_tests_properties(aowis-dvmdb-object-id-tests PROPERTIES LABELS "aowis;identity")
    add_executable(aowis-dvmdb-working-copy-tests
        tests/native_working_copy_tests.cpp)
    target_link_libraries(aowis-dvmdb-working-copy-tests PRIVATE aowis-dvmdb-project-service)
    add_test(NAME aowis-dvmdb-working-copy-tests
        COMMAND aowis-dvmdb-working-copy-tests)
    set_tests_properties(aowis-dvmdb-working-copy-tests PROPERTIES
        LABELS "aowis;working-copy" TIMEOUT 30)
    add_executable(aowis-dvmdb-local-commit-tests
        tests/native_local_commit_tests.cpp)
    target_link_libraries(aowis-dvmdb-local-commit-tests PRIVATE aowis-dvmdb-project-service Threads::Threads)
    add_test(NAME aowis-dvmdb-local-commit-tests
        COMMAND aowis-dvmdb-local-commit-tests)
    set_tests_properties(aowis-dvmdb-local-commit-tests PROPERTIES
        LABELS "aowis;working-copy;durability;identity" TIMEOUT 120)
    add_executable(aowis-dvmdb-canonical-project-service-tests
        tests/native_canonical_project_service_tests.cpp)
    target_link_libraries(aowis-dvmdb-canonical-project-service-tests PRIVATE aowis-dvmdb-project-service)
    add_test(NAME aowis-dvmdb-canonical-project-service-tests
        COMMAND aowis-dvmdb-canonical-project-service-tests)
    set_tests_properties(aowis-dvmdb-canonical-project-service-tests PROPERTIES LABELS "aowis;identity")

    add_executable(aowis-dvmdb-project-service-tests tests/project_service_tests.cpp)
    target_link_libraries(aowis-dvmdb-project-service-tests PRIVATE aowis-dvmdb-project-service)
    add_test(NAME aowis-dvmdb-project-service-tests COMMAND aowis-dvmdb-project-service-tests)
    set_tests_properties(aowis-dvmdb-project-service-tests PROPERTIES LABELS "aowis")
    find_package(Threads REQUIRED)
    # Full-service cold backup/restore drill, including independent repository locks.
    add_executable(aowis-dvmdb-offline-backup-tests tests/native_offline_backup_tests.cpp)
    target_link_libraries(aowis-dvmdb-offline-backup-tests PRIVATE aowis-dvmdb-project-service)
    add_dependencies(aowis-dvmdb-offline-backup-tests aowis-dvmdb-verify)
    add_test(NAME aowis-dvmdb-offline-backup-tests
        COMMAND aowis-dvmdb-offline-backup-tests
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/offline_backup.py"
            "$<TARGET_FILE:aowis-dvmdb-verify>")
    set_tests_properties(aowis-dvmdb-offline-backup-tests PROPERTIES LABELS "aowis")
    # UUIDv7-only offline server-root move/restart: divergent project histories,
    # large current-format model data, a commit after restore and second backup.
    add_executable(aowis-dvmdb-upgrade-restore-tests
        tests/native_upgrade_restore_tests.cpp)
    target_link_libraries(aowis-dvmdb-upgrade-restore-tests PRIVATE aowis-dvmdb-project-service)
    add_dependencies(aowis-dvmdb-upgrade-restore-tests aowis-dvmdb-verify)
    add_test(NAME aowis-dvmdb-upgrade-restore-tests
        COMMAND aowis-dvmdb-upgrade-restore-tests
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/offline_backup.py"
            "$<TARGET_FILE:aowis-dvmdb-verify>")
    set_tests_properties(aowis-dvmdb-upgrade-restore-tests PROPERTIES
        LABELS "aowis;identity;durability" TIMEOUT 180)
    add_executable(aowis-dvmdb-project-concurrency-tests tests/project_concurrency_tests.cpp)
    target_link_libraries(aowis-dvmdb-project-concurrency-tests PRIVATE aowis-dvmdb-project-service Threads::Threads)
    add_test(NAME aowis-dvmdb-project-concurrency-tests COMMAND aowis-dvmdb-project-concurrency-tests)
    set_tests_properties(aowis-dvmdb-project-concurrency-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-pack-operational-stress-tests
        tests/native_pack_operational_stress_tests.cpp)
    target_link_libraries(aowis-dvmdb-pack-operational-stress-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-pack-operational-stress-tests
        COMMAND aowis-dvmdb-pack-operational-stress-tests)
    set_tests_properties(aowis-dvmdb-pack-operational-stress-tests PROPERTIES LABELS "aowis")
    # True kernel ENOSPC on a disposable, 32-MiB private tmpfs. The launcher
    # skips cleanly on hosts without unprivileged user/mount namespaces.
    add_executable(aowis-dvmdb-physical-enospc-tests
        tests/native_physical_enospc_tests.cpp)
    target_link_libraries(aowis-dvmdb-physical-enospc-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-physical-enospc-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/physical_enospc_mount.py"
            --binary "$<TARGET_FILE:aowis-dvmdb-physical-enospc-tests>")
    set_tests_properties(aowis-dvmdb-physical-enospc-tests PROPERTIES
        LABELS "aowis;physical-enospc" SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_executable(aowis-dvmdb-recovery-tests tests/native_recovery_tests.cpp)
    target_link_libraries(aowis-dvmdb-recovery-tests PRIVATE aowis-dvmdb-store-fault-tests)
    add_test(NAME aowis-dvmdb-recovery-tests COMMAND aowis-dvmdb-recovery-tests)
    set_tests_properties(aowis-dvmdb-recovery-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-scratch-tests tests/native_scratch_tests.cpp)
    target_link_libraries(aowis-dvmdb-scratch-tests PRIVATE aowis-dvmdb-store-fault-tests)
    add_test(NAME aowis-dvmdb-scratch-tests COMMAND aowis-dvmdb-scratch-tests)
    set_tests_properties(aowis-dvmdb-scratch-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-error-kind-tests tests/native_error_kind_tests.cpp)
    target_link_libraries(aowis-dvmdb-error-kind-tests PRIVATE aowis-dvmdb-store-fault-tests)
    add_test(NAME aowis-dvmdb-error-kind-tests COMMAND aowis-dvmdb-error-kind-tests)
    set_tests_properties(aowis-dvmdb-error-kind-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-sync-cli-tests tests/native_sync_cli_tests.cpp)
    target_link_libraries(aowis-dvmdb-sync-cli-tests PRIVATE aowis-dvmdb-store)
    add_dependencies(aowis-dvmdb-sync-cli-tests aowis-dvmdb-sync)
    add_test(NAME aowis-dvmdb-sync-cli-tests COMMAND aowis-dvmdb-sync-cli-tests
        $<TARGET_FILE:aowis-dvmdb-sync>)
    set_tests_properties(aowis-dvmdb-sync-cli-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-access-tests tests/native_access_tests.cpp)
    target_link_libraries(aowis-dvmdb-access-tests PRIVATE aowis-dvmdb-store)
    add_dependencies(aowis-dvmdb-access-tests aowis-dvmdb-access)
    add_test(NAME aowis-dvmdb-access-tests COMMAND aowis-dvmdb-access-tests $<TARGET_FILE:aowis-dvmdb-access>)
    set_tests_properties(aowis-dvmdb-access-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-merkle-integrity-tests tests/native_merkle_integrity_tests.cpp)
    target_link_libraries(aowis-dvmdb-merkle-integrity-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-merkle-integrity-tests COMMAND aowis-dvmdb-merkle-integrity-tests)
    set_tests_properties(aowis-dvmdb-merkle-integrity-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-model-import-integrity-tests
        tests/native_model_import_integrity_tests.cpp)
    target_link_libraries(aowis-dvmdb-model-import-integrity-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-model-import-integrity-tests
        COMMAND aowis-dvmdb-model-import-integrity-tests)
    set_tests_properties(aowis-dvmdb-model-import-integrity-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-repository-verify-tests
        tests/native_repository_verify_tests.cpp)
    target_link_libraries(aowis-dvmdb-repository-verify-tests PRIVATE aowis-dvmdb-store)
    add_dependencies(aowis-dvmdb-repository-verify-tests aowis-dvmdb-verify)
    add_test(NAME aowis-dvmdb-repository-verify-tests
        COMMAND aowis-dvmdb-repository-verify-tests $<TARGET_FILE:aowis-dvmdb-verify>)
    set_tests_properties(aowis-dvmdb-repository-verify-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-bundle-import-profile-tests
        tests/native_bundle_import_profile_tests.cpp)
    target_link_libraries(aowis-dvmdb-bundle-import-profile-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-bundle-import-profile-tests
        COMMAND aowis-dvmdb-bundle-import-profile-tests)
    set_tests_properties(aowis-dvmdb-bundle-import-profile-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-bundle-limit-tests tests/native_bundle_limit_tests.cpp)
    target_link_libraries(aowis-dvmdb-bundle-limit-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-bundle-limit-tests COMMAND aowis-dvmdb-bundle-limit-tests)
    set_tests_properties(aowis-dvmdb-bundle-limit-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-bundle-disk-index-tests tests/native_bundle_disk_index_tests.cpp)
    target_link_libraries(aowis-dvmdb-bundle-disk-index-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-bundle-disk-index-tests COMMAND aowis-dvmdb-bundle-disk-index-tests)
    set_tests_properties(aowis-dvmdb-bundle-disk-index-tests PROPERTIES LABELS "aowis")
    # Separate, cheap cross-kind trie-cache regression; also useful under
    # sanitizers without executing the long large-bundle stress fixture.
    add_test(NAME aowis-dvmdb-bundle-shared-trie-tests
        COMMAND aowis-dvmdb-bundle-disk-index-tests --shared-kinds-only)
    set_tests_properties(aowis-dvmdb-bundle-shared-trie-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-bundle-revision-marks-tests
        tests/native_bundle_revision_marks_tests.cpp)
    target_link_libraries(aowis-dvmdb-bundle-revision-marks-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-bundle-revision-marks-tests
        COMMAND aowis-dvmdb-bundle-revision-marks-tests)
    set_tests_properties(aowis-dvmdb-bundle-revision-marks-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-sync-tests tests/native_sync_tests.cpp)
    target_link_libraries(aowis-dvmdb-sync-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-sync-tests COMMAND aowis-dvmdb-sync-tests)
    set_tests_properties(aowis-dvmdb-sync-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-merge-ancestry-tests tests/native_merge_ancestry_tests.cpp)
    target_link_libraries(aowis-dvmdb-merge-ancestry-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-merge-ancestry-tests COMMAND aowis-dvmdb-merge-ancestry-tests)
    set_tests_properties(aowis-dvmdb-merge-ancestry-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-incremental-sync-tests tests/native_incremental_sync_tests.cpp)
    target_link_libraries(aowis-dvmdb-incremental-sync-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-incremental-sync-tests COMMAND aowis-dvmdb-incremental-sync-tests)
    set_tests_properties(aowis-dvmdb-incremental-sync-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-diff-scope-tests tests/native_diff_scope_tests.cpp)
    target_link_libraries(aowis-dvmdb-diff-scope-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-diff-scope-tests COMMAND aowis-dvmdb-diff-scope-tests)
    set_tests_properties(aowis-dvmdb-diff-scope-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-dvmdb-store-tests tests/native_store_tests.cpp)
    target_link_libraries(aowis-dvmdb-store-tests PRIVATE aowis-dvmdb-store)
    add_test(NAME aowis-dvmdb-store-tests COMMAND aowis-dvmdb-store-tests)
    set_tests_properties(aowis-dvmdb-store-tests PROPERTIES LABELS "aowis")
    add_custom_target(aowis-dvmdb-tests DEPENDS aowis-dvmdb-local-commit-tests aowis-dvmdb-working-copy-tests aowis-dvmdb-canonical-project-service-tests aowis-dvmdb-revision-bundle-large-tests aowis-dvmdb-revision-bundle-tests aowis-dvmdb-revision-graph-tests aowis-dvmdb-merge-tests aowis-dvmdb-project-model-tests aowis-dvmdb-packed-revision-tests aowis-dvmdb-revision-resolver-tests aowis-dvmdb-revision-record-tests aowis-dvmdb-branch-id-tests aowis-dvmdb-object-id-tests aowis-dvmdb-vm-powercut-worker aowis-dvmdb-sigkill-restart-tests aowis-dvmdb-physical-enospc-tests aowis-dvmdb-offline-backup-tests aowis-dvmdb-transfer-staging-budget-tests aowis-dvmdb-index-scaling-benchmark aowis-dvmdb-pack-operational-stress-tests aowis-dvmdb-bundle-import-profile-tests aowis-dvmdb-replication-scaling-benchmark aowis-dvmdb-bundle-revision-marks-tests aowis-dvmdb-bundle-disk-index-tests aowis-dvmdb-merge-ancestry-tests aowis-dvmdb-bundle-limit-tests aowis-dvmdb-project-service-tests aowis-dvmdb-project-concurrency-tests aowis-dvmdb-recovery-tests aowis-dvmdb-scratch-tests aowis-dvmdb-error-kind-tests aowis-dvmdb-repository-verify-tests aowis-dvmdb-model-import-integrity-tests aowis-dvmdb-merkle-integrity-tests aowis-dvmdb-store-tests aowis-dvmdb-diff-scope-tests aowis-dvmdb-incremental-sync-tests aowis-dvmdb-sync-tests aowis-dvmdb-sync-cli-tests aowis-dvmdb-access-tests)
endif()

# Canonical AOWIS model serialization lives in AOWIS-SERVER-MODEL.
# This optional layer contains only DVMDB storage adapters and benchmarks.
# The standalone byte/object store can still build without Qt.
option(AOWIS_DVMDB_WITH_MODEL_CODEC "Build native model serialization and hydraulic persistence" ON)
if(AOWIS_DVMDB_WITH_MODEL_CODEC)
    find_package(Qt6 REQUIRED COMPONENTS Core)
    if(Qt6Core_FOUND)
        set(AOWIS_DVMDB_MODEL_SOURCE_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL"
            CACHE PATH "Pinned AOWIS model submodule")
        if(NOT EXISTS "${AOWIS_DVMDB_MODEL_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR "Missing Model submodule. Run ./git_submodule_init.sh")
        endif()
        if(NOT TARGET AOWIS::Model)
            add_subdirectory("${AOWIS_DVMDB_MODEL_SOURCE_DIR}"
                "${CMAKE_CURRENT_BINARY_DIR}/aowis-server-model")
        endif()
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ModelCodecFingerprint.cmake")
        aowis_generate_model_codec_fingerprint("${AOWIS_DVMDB_MODEL_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/model-fingerprint")
        # Canonical serialization, value checks and field-aware merges are
        # compiled exactly once in AOWIS::Model, regardless of host target.
        add_library(aowis-dvmdb-hydraulic STATIC
            src/model_codec_adapter.cpp
            src/hydraulic_network_persistence.cpp)
        add_library(AOWIS::DvmdbHydraulic ALIAS aowis-dvmdb-hydraulic)
        target_compile_features(aowis-dvmdb-hydraulic PUBLIC cxx_std_20)
        target_include_directories(aowis-dvmdb-hydraulic PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        # Hydraulic revision-action coverage invokes ProjectService directly.
        # Export that dependency to every consumer (HTTP, tests, benchmarks),
        # with the service archive after the hydraulic archive at link time.
        target_link_libraries(aowis-dvmdb-hydraulic PUBLIC
            aowis-dvmdb-project-service aowis-dvmdb-store AOWIS::Model)
        add_executable(aowis-dvmdb-hydraulic-model-benchmark
            benchmarks/v5_model_benchmark.cpp)
        target_link_libraries(aowis-dvmdb-hydraulic-model-benchmark PRIVATE aowis-dvmdb-hydraulic)
        add_executable(aowis-dvmdb-hydraulic-edit-benchmark
            benchmarks/hydraulic_edit_benchmark.cpp)
        target_link_libraries(aowis-dvmdb-hydraulic-edit-benchmark PRIVATE aowis-dvmdb-hydraulic)
        if(BUILD_TESTING)
            add_dependencies(aowis-dvmdb-tests aowis-dvmdb-hydraulic-edit-benchmark aowis-dvmdb-verify)
            # Genuine canonical hydraulic model operations and a fresh-process
            # full verification. Each invocation creates its own disposable root.
            add_test(NAME aowis-dvmdb-hydraulic-workload-smoke
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/qualify_hydraulic_workload.py"
                    --binary "$<TARGET_FILE:aowis-dvmdb-hydraulic-edit-benchmark>"
                    --verifier "$<TARGET_FILE:aowis-dvmdb-verify>"
                    --pipes 12 --edits 2 --topology-edits 2 --repeats 1
                    --results-root "${CMAKE_CURRENT_BINARY_DIR}/hydraulic-qualification")
            set_tests_properties(aowis-dvmdb-hydraulic-workload-smoke PROPERTIES
                LABELS "aowis;qualification;hydraulic" TIMEOUT 120)
        endif()
        if(BUILD_TESTING)
            add_executable(aowis-dvmdb-model-codec-tests tests/v5_codec_tests.cpp)
            target_link_libraries(aowis-dvmdb-model-codec-tests PRIVATE aowis-dvmdb-hydraulic)
            add_test(NAME aowis-dvmdb-model-codec-tests COMMAND aowis-dvmdb-model-codec-tests)
            set_tests_properties(aowis-dvmdb-model-codec-tests PROPERTIES LABELS "aowis")
            add_executable(aowis-dvmdb-hydraulic-network-tests
                tests/hydraulic_network_persistence_tests.cpp)
            target_link_libraries(aowis-dvmdb-hydraulic-network-tests PRIVATE aowis-dvmdb-hydraulic)
            add_test(NAME aowis-dvmdb-hydraulic-network-tests COMMAND aowis-dvmdb-hydraulic-network-tests)
            set_tests_properties(aowis-dvmdb-hydraulic-network-tests PROPERTIES LABELS "aowis")
            add_executable(aowis-dvmdb-model-value-rules-tests tests/model_value_rules_tests.cpp)
            target_link_libraries(aowis-dvmdb-model-value-rules-tests PRIVATE aowis-dvmdb-hydraulic)
            get_target_property(AOWIS_DVMDB_MODEL_ENUM_DIR AOWIS::Model AOWIS_MODEL_ENUM_DIR)
            if(NOT AOWIS_DVMDB_MODEL_ENUM_DIR)
                message(FATAL_ERROR "MODEL target must expose the enum verification directory")
            endif()
            target_include_directories(aowis-dvmdb-model-value-rules-tests PRIVATE "${AOWIS_DVMDB_MODEL_ENUM_DIR}")
            add_test(NAME aowis-dvmdb-model-value-rules-tests COMMAND aowis-dvmdb-model-value-rules-tests)
            set_tests_properties(aowis-dvmdb-model-value-rules-tests PROPERTIES LABELS "aowis")
            add_dependencies(aowis-dvmdb-tests aowis-dvmdb-model-codec-tests aowis-dvmdb-hydraulic-network-tests
                aowis-dvmdb-model-value-rules-tests)
        endif()
        message(STATUS "Native model serializer and hydraulic persistence enabled")
    else()
        message(STATUS "Native model serialization disabled: Qt6 Core development package not found")
    endif()
else()
    message(STATUS "Native model serialization disabled by configuration")
endif()

# HTTP transport: vendored cpp-httplib (external/cpp-httplib, pinned single
# header) on a worker thread pool. Qt Core is used for JSON/CLI/INI and, when
# enabled, the hydraulic model adapter. Qt Network/HttpServer are not used.
# A native-only build uses -DAOWIS_DVMDB_WITH_MODEL_CODEC=OFF (HTTP defaults
# to OFF as well).
option(AOWIS_DVMDB_WITH_HTTP_SERVER "Build the HTTP/TCP multi-project server" ${AOWIS_DVMDB_WITH_MODEL_CODEC})
if(AOWIS_DVMDB_WITH_HTTP_SERVER)
    find_package(Qt6 REQUIRED COMPONENTS Core)
    find_package(Threads REQUIRED)
    add_library(aowis-cpp-httplib INTERFACE)
    target_include_directories(aowis-cpp-httplib SYSTEM INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/external/cpp-httplib")
    target_link_libraries(aowis-cpp-httplib INTERFACE Threads::Threads)
    add_library(aowis-dvmdb-http STATIC src/dvmdb_http_routes.cpp)
    target_compile_features(aowis-dvmdb-http PUBLIC cxx_std_20)
    target_include_directories(aowis-dvmdb-http PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    if(NOT AOWIS_DVMDB_WITH_MODEL_CODEC)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/model-fingerprint")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/model-fingerprint/aowis_model_codec_fingerprint.h"
            "#pragma once\n#define AOWIS_MODEL_CODEC_FINGERPRINT \"unavailable\"\n")
    endif()
    target_include_directories(aowis-dvmdb-http PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/model-fingerprint")
    target_link_libraries(aowis-dvmdb-http PUBLIC aowis-dvmdb-project-service aowis-cpp-httplib Qt6::Core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(aowis-dvmdb-http PRIVATE -Wall -Wextra)
    endif()
    if(TARGET aowis-dvmdb-hydraulic)
        target_link_libraries(aowis-dvmdb-http PUBLIC aowis-dvmdb-hydraulic)
        target_compile_definitions(aowis-dvmdb-http PUBLIC AOWIS_DVMDB_HTTP_WITH_HYDRAULIC=1)
    endif()

    add_executable(aowis-server-dvmdb tools/aowis_server_dvmdb.cpp)
    target_link_libraries(aowis-server-dvmdb PRIVATE aowis-dvmdb-http)
    if(BUILD_TESTING)
        add_executable(aowis-dvmdb-http-tests tests/native_http_tests.cpp)
        target_link_libraries(aowis-dvmdb-http-tests PRIVATE aowis-dvmdb-http)
        target_include_directories(aowis-dvmdb-http-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/model-fingerprint")
        add_dependencies(aowis-dvmdb-http-tests aowis-server-dvmdb)
        # Real-server validation of HTTP readiness, the XDG default,
        # automatic legacy-root reuse and explicit CLI/INI overrides.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME aowis-dvmdb-default-data-root-tests
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/server_default_data_root_tests.py"
                    --server "$<TARGET_FILE:aowis-server-dvmdb>")
            set_tests_properties(aowis-dvmdb-default-data-root-tests PROPERTIES
                LABELS "aowis;qualification" TIMEOUT 75)
        endif()
        add_test(NAME aowis-dvmdb-http-tests COMMAND aowis-dvmdb-http-tests
            $<TARGET_FILE:aowis-server-dvmdb>)
        set_tests_properties(aowis-dvmdb-http-tests PROPERTIES LABELS "aowis")
        add_dependencies(aowis-dvmdb-tests aowis-dvmdb-http-tests)
        # Separate real-process transfer/admission stress: independent of Qt
        # test helpers, but runs against the production HTTP executable.
        add_executable(aowis-dvmdb-http-transfer-stress-tests
            tests/native_http_transfer_stress_tests.cpp)
        target_link_libraries(aowis-dvmdb-http-transfer-stress-tests PRIVATE
            aowis-dvmdb-store aowis-cpp-httplib)
        add_dependencies(aowis-dvmdb-http-transfer-stress-tests aowis-server-dvmdb)
        add_test(NAME aowis-dvmdb-http-stress-fixture-tests
            COMMAND aowis-dvmdb-http-transfer-stress-tests --fixture-only)
        set_tests_properties(aowis-dvmdb-http-stress-fixture-tests PROPERTIES
            LABELS "aowis;http-stress" TIMEOUT 45)
        add_test(NAME aowis-dvmdb-http-transfer-stress-tests
            COMMAND aowis-dvmdb-http-transfer-stress-tests $<TARGET_FILE:aowis-server-dvmdb>)
        set_tests_properties(aowis-dvmdb-http-transfer-stress-tests PROPERTIES
            LABELS "aowis;http-stress" TIMEOUT 180 RUN_SERIAL TRUE)
        # Short end-to-end soak guarantees elapsed-time checkpoints really
        # appear, rather than validating only synthetic progress-log fixtures.
        add_test(NAME aowis-dvmdb-http-soak-progress-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_soak_progress_tests.py"
                "$<TARGET_FILE:aowis-dvmdb-http-transfer-stress-tests>"
                "$<TARGET_FILE:aowis-server-dvmdb>")
        set_tests_properties(aowis-dvmdb-http-soak-progress-tests PROPERTIES
            LABELS "aowis;http-stress;qualification" TIMEOUT 100 RUN_SERIAL TRUE)
        add_dependencies(aowis-dvmdb-tests aowis-dvmdb-http-transfer-stress-tests)
    endif()
    message(STATUS "Native multi-project HTTP server enabled (cpp-httplib)")
else()
    message(STATUS "Native multi-project HTTP server disabled by configuration")
endif()

# Keep the aggregate test build in sync with registered CTest executables.
# compile_linux_tests.sh builds this target before invoking CTest; an omitted
# dependency would otherwise leave a registered test as "Not Run". The explicit
# dependencies above still cover helper executables invoked by script tests.
if(BUILD_TESTING)
    get_property(AOWIS_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
    foreach(AOWIS_REGISTERED_TEST IN LISTS AOWIS_REGISTERED_TESTS)
        if(TARGET "${AOWIS_REGISTERED_TEST}")
            get_target_property(AOWIS_TEST_TARGET_TYPE "${AOWIS_REGISTERED_TEST}" TYPE)
            if(AOWIS_TEST_TARGET_TYPE STREQUAL "EXECUTABLE")
                add_dependencies(aowis-dvmdb-tests "${AOWIS_REGISTERED_TEST}")
            endif()
        endif()
    endforeach()
endif()
