# Runs one JSON scenario: replays INPUT twice with REPLAY, checks both runs are identical
# (determinism), then compares against EXPECTED.
#   cmake -DREPLAY=... -DINPUT=... -DEXPECTED=... -DOUTPUT=... -P RunScenario.cmake
foreach(var REPLAY INPUT EXPECTED OUTPUT)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is not defined")
  endif()
endforeach()

get_filename_component(out_dir ${OUTPUT} DIRECTORY)
file(MAKE_DIRECTORY ${out_dir})

foreach(run 1 2)
  execute_process(
    COMMAND ${REPLAY} ${INPUT}
    OUTPUT_FILE ${OUTPUT}.${run}
    ERROR_VARIABLE stderr
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "replay failed (rc=${rc}): ${stderr}")
  endif()
endforeach()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUTPUT}.1 ${OUTPUT}.2 RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "non-deterministic output: ${OUTPUT}.1 differs from ${OUTPUT}.2")
endif()
file(RENAME ${OUTPUT}.1 ${OUTPUT})
file(REMOVE ${OUTPUT}.2)

if(NOT EXISTS ${EXPECTED})
  message(FATAL_ERROR "missing expected file ${EXPECTED}; actual output is in ${OUTPUT}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${EXPECTED} ${OUTPUT} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  execute_process(COMMAND diff -u ${EXPECTED} ${OUTPUT} OUTPUT_VARIABLE diff_out)
  message(FATAL_ERROR "output differs from expected:\n${diff_out}")
endif()
