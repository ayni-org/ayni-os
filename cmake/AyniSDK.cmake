# Source-level SDK entry point. Platform composition remains the consumer's choice.
# Check before the include guard: two checkouts must not silently share targets.
get_filename_component(_ayni_sdk_root "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
get_property(_ayni_sdk_existing_root GLOBAL PROPERTY AYNI_SDK_SOURCE_ROOT)
if(_ayni_sdk_existing_root AND NOT _ayni_sdk_existing_root STREQUAL _ayni_sdk_root)
    message(FATAL_ERROR "One build cannot mix Ayni sources '${_ayni_sdk_existing_root}' and '${_ayni_sdk_root}'")
endif()
set_property(GLOBAL PROPERTY AYNI_SDK_SOURCE_ROOT "${_ayni_sdk_root}")
include_guard(GLOBAL)

# Components are a closed list, not a plugin registry. Dependencies below name
# implementations needed by static-library consumers. Optional platform libraries
# define targets only; the runtime/consumer still composes each deployment.
function(ayni_add_sdk)
    cmake_parse_arguments(PARSE_ARGV 0 sdk "" "" "COMPONENTS")
    if(sdk_UNPARSED_ARGUMENTS OR sdk_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "ayni_add_sdk expects COMPONENTS followed by component names")
    endif()
    set(known api core strategies software_provider permit_gate assistant
        software_workflow mock_provider mock_execution host_storage native_host fixture_model conversation portable_profile model_protocol operation_session)
    if(NOT sdk_COMPONENTS)
        set(sdk_COMPONENTS core strategies)
    endif()
    foreach(component IN LISTS sdk_COMPONENTS)
        if(NOT component IN_LIST known)
            message(FATAL_ERROR "Unknown Ayni component '${component}'; available: ${known}")
        endif()
    endforeach()

    get_property(root GLOBAL PROPERTY AYNI_SDK_SOURCE_ROOT)
    include("${root}/cmake/AyniCommon.cmake")
    set(selected api ${sdk_COMPONENTS})
    foreach(component IN LISTS sdk_COMPONENTS)
        if(NOT component STREQUAL api AND NOT component STREQUAL permit_gate)
            list(APPEND selected core)
        endif()
        if(component STREQUAL native_host)
            list(APPEND selected host_storage)
        endif()
        if(component STREQUAL mock_execution)
            list(APPEND selected permit_gate)
        endif()
    endforeach()

    if(conversation IN_LIST selected OR portable_profile IN_LIST selected)
        list(APPEND selected strategies host_storage conversation)
    endif()
    if(portable_profile IN_LIST selected)
        list(APPEND selected fixture_model software_provider software_workflow mock_execution permit_gate)
    endif()

    # Fixed topological order also permits repeated calls that add components.
    set(paths "" core strategies/builtin providers/software control/permit_gate
        applications/assistant applications/software_workflow providers/mock providers/mock_execution
        hosts/storage hosts/native providers/fixture_model runtime/conversation runtime/portable providers/model_protocol runtime/operation_session)
    set(targets ayni_api ayni_core ayni_strategies ayni_provider_software ayni_control_gate
        ayni_assistant_application ayni_software_workflow ayni_provider_mock ayni_provider_mock_execution
        ayni_host_storage ayni_host_native ayni_provider_fixture_model ayni_runtime_conversation ayni_runtime_portable ayni_model_protocol ayni_runtime_operation_session)
    set(aliases Api Core Strategies SoftwareProvider PermitGate Assistant
        SoftwareWorkflow MockProvider MockExecution HostStorage NativeHost FixtureModel Conversation PortableProfile ModelProtocol OperationSession)
    foreach(component IN LISTS known)
        if(NOT component IN_LIST selected)
            continue()
        endif()
        list(FIND known "${component}" index)
        list(GET targets ${index} target)
        list(GET paths ${index} path)
        list(GET aliases ${index} alias)
        if(NOT TARGET ${target})
            add_subdirectory("${root}/${path}" "${CMAKE_CURRENT_BINARY_DIR}/ayni-sdk/${component}")
        endif()
        if(NOT TARGET Ayni::${alias})
            add_library(Ayni::${alias} ALIAS ${target})
        endif()
    endforeach()
endfunction()
