if(NOT DEFINED ENGINE_EXECUTABLE OR NOT EXISTS "${ENGINE_EXECUTABLE}")
  message(FATAL_ERROR "vividcam_engine executable was not provided: ${ENGINE_EXECUTABLE}")
endif()

execute_process(
  COMMAND "${ENGINE_EXECUTABLE}"
          --run-for-ms 80
          --heartbeat-ms 10
          --instance-id ctest
          --quiet
  RESULT_VARIABLE engine_result
  OUTPUT_VARIABLE engine_output
  ERROR_VARIABLE engine_error
  TIMEOUT 5
)

message(STATUS "vividcam_engine output:\n${engine_output}")
if(NOT "${engine_result}" STREQUAL "0")
  message(FATAL_ERROR
    "vividcam_engine exited with ${engine_result}\n${engine_error}")
endif()

if(NOT "${engine_output}" MATCHES
    "event=lifecycle[^\n]*state=stopped[^\n]*stop_reason=run-for")
  message(FATAL_ERROR
    "vividcam_engine did not emit the expected final stopped/run-for status")
endif()
