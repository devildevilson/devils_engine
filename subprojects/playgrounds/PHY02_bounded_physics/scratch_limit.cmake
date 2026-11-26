execute_process(
  COMMAND "${PROBE}" --scratch-limit-probe
  RESULT_VARIABLE result
  ERROR_VARIABLE diagnostic
  TIMEOUT 10)
if(result STREQUAL "0" OR result MATCHES "[Tt]imeout" OR
   NOT diagnostic MATCHES "PHY02 solver scratch: used [0-9]+ [+] request [0-9]+ bytes exceeds limit 1")
  message(FATAL_ERROR "Expected a solver scratch refusal, got ${result}\n${diagnostic}")
endif()
