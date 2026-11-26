# CTest loads this after GoogleTest discovery. Wall-clock benchmarks must not
# compete with other tests in the same run, including sanitizer process startup.
# Keep their existing limits and all samples; RUN_SERIAL does not claim an RT host.
set(host_discovered_tests ${ayni_unit_tests_TESTS}
    ${ayni_esp32_chat_integration_tests_TESTS} ${ayni_esp32_motor_execution_tests_TESTS})
foreach(host_latency_test IN ITEMS
    ControlSessionTest.LocalLoopWallLatencyMeetsHostBudget
    ReflexParameters.DynamicProjectionRechecksScopeAndMeetsHostLatencyBudget
    ControlMailbox.TargetReceiptWallLatencyMeetsHostBudget
    MotorWorker.SoftwareHotSameDirectionReverseAndHoldHaveBoundedMeasuredLatency
    TurnDiagnostics.OwnerWallLatencyReportsShortMediumAndLongHistories)
    list(FIND host_discovered_tests "${host_latency_test}" host_latency_index)
    if(host_latency_index GREATER_EQUAL 0)
        set_tests_properties("${host_latency_test}" PROPERTIES RUN_SERIAL TRUE LABELS host_latency)
    endif()
endforeach()
