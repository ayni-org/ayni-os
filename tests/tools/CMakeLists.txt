add_test(NAME check_deps_rules
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_check_deps
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME check_deps_repository
    COMMAND "${Python3_EXECUTABLE}" "${AYNI_ROOT}/tools/check_deps.py" --root "${AYNI_ROOT}")
add_test(NAME check_contracts_rules
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_check_contracts
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME check_contracts_repository
    COMMAND "${Python3_EXECUTABLE}" "${AYNI_ROOT}/tools/check_contracts.py" --root "${AYNI_ROOT}")

# ayni_add_core_module enforces the layer order at configure time. Each case
# configures a probe project; rejections must name the offending dependency.
# dependencies is comma-separated.
function(ayni_layer_case name module dependencies expected)
    set(binary_dir "${CMAKE_CURRENT_BINARY_DIR}/cmake_layers/${name}")
    add_test(NAME ${name}
        COMMAND "${CMAKE_COMMAND}"
            -S "${CMAKE_CURRENT_SOURCE_DIR}/cmake_layers" -B "${binary_dir}"
            -G "${CMAKE_GENERATOR}"
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            "-DAYNI_REPOSITORY=${AYNI_ROOT}" "-DPROBE_MODULE=${module}" "-DPROBE_DEPENDS=${dependencies}")
    if(expected)
        set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected}")
    endif()
endfunction()

ayni_layer_case(layers_accept_lower_layer_dependency work "authority,foundation" "")
ayni_layer_case(layers_reject_upward_dependency foundation kernel "may not depend on core/kernel")
ayni_layer_case(layers_reject_same_layer_dependency authority budget "may not depend on core/budget")
ayni_layer_case(layers_reject_unknown_module scheduler "" "core/scheduler is not a module")
ayni_layer_case(layers_reject_unknown_dependency work scheduler "depends on unknown module 'scheduler'")
