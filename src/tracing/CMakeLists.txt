# we are in "src/tracing", so create the output dir manually.
# the source files include the tracing headers like
# #include "tracing/oprequest.h". so better put them into
# ${PROJECT_BINARY_DIR}/include, where acconfig.h is also located
set(working_dir ${CMAKE_BINARY_DIR}/include)
set(header_dir ${working_dir}/tracing)
set(tracing_build_dir ${CMAKE_BINARY_DIR}/tracing)
file(MAKE_DIRECTORY ${header_dir})
file(MAKE_DIRECTORY ${tracing_build_dir})

add_custom_target(tracepoint_libraries)

file(GLOB tracesources "tracetool/subsys/*")
foreach(tracesource ${tracesources})
  get_filename_component(name ${tracesource} NAME_WE)
  set(tpfile ${tracesource}.tp)
  set(logging_tps "${logging_tps} ${name}.tp")

  execute_process(
    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/tracetool/tracetool.py -f ${tracesource} -b lttng -t tp
    OUTPUT_FILE ${tracing_build_dir}/${name}.tp)
  execute_process(
    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/tracetool/tracetool.py -f ${tracesource} -b lttng -t c
    OUTPUT_FILE ${tracing_build_dir}/${name}.c)
  execute_process(
    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/tracetool/tracetool.py -f ${tracesource} -b lttng -t h
    OUTPUT_FILE ${header_dir}/${name}_impl.h)
#  list(APPEND link_tracepoint_libs ${name}_tp)

  # TODO this part isn't working yet
#  add_custom_command(
#    OUTPUT ${tracing_build_dir}/${name}.tp
#    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/tracetool/tracetool.py -f ${tracesource} -b lttng -t tp > ${tracing_build_dir}/${name}.tp
#    DEPENDS ${tracesource})
#  add_custom_command(
#    OUTPUT ${tracing_build_dir}/${name}.c
#    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/tracetool/tracetool.py -f ${tracesource} -b lttng -t c > ${tracing_build_dir}/${name}.c
#    DEPENDS ${tracesource})
#  add_custom_command(
#    OUTPUT ${header_dir}/${name}_impl.h
#    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/tracetool/tracetool.py -f ${tracesource} -b lttng -t h > ${header_dir}/${name}_impl.h
#    DEPENDS ${tracesource})
endforeach()

file(GLOB generated_tps "${tracing_build_dir}/*.tp")
file(GLOB tps "*.tp")
foreach(tp ${tps} ${generated_tps})
  get_filename_component(name ${tp} NAME_WE)
  set(header ${header_dir}/${name}.h)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${LTTNG_GEN_TP} ${tp} -o tracing/${name}.h
    DEPENDS ${tp}
    WORKING_DIRECTORY ${working_dir}
    COMMENT "generating ${header}")
  add_custom_target(
    ${name}-tp
    DEPENDS ${header})
endforeach()

function(add_tracing_library name tracings version)
  foreach(tp_file ${tracings})
    get_filename_component(tp ${tp_file} NAME_WE)
    list(APPEND hdrs
      ${header_dir}/${tp}.h)
    # TODO: remove this condition when we move to tracetool for existing tracepoints
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${tp}.c")
      list(APPEND tpfiles ${tp}.c)
    else()
      list(APPEND tpfiles ${tracing_build_dir}/${tp}.c)
    endif()
  endforeach()
  add_library(${name} SHARED ${hdrs} ${tpfiles})
  target_link_libraries(${name} ${LTTNGUST_LIBRARIES} ${CMAKE_DL_LIBS})
  string(REGEX MATCH "^[0-9]+" soversion ${version})
  set_target_properties(${name} PROPERTIES
    OUTPUT_NAME ${name}
    VERSION ${version}
    SOVERSION ${soversion}
    INSTALL_RPATH "")
  add_dependencies(tracepoint_libraries ${name})
endfunction()

set(osd_traces oprequest.tp osd.tp pg.tp)
add_tracing_library(osd_tp "${osd_traces}" 1.0.0)
add_tracing_library(rados_tp librados.tp 2.0.0)
add_tracing_library(os_tp objectstore.tp 1.0.0)
add_tracing_library(rgw_op_tp rgw_op.tp 1.0.0)
add_tracing_library(rgw_rados_tp rgw_rados.tp 1.0.0)
add_tracing_library(ceph_logging_tp ceph_logging.tp 1.0.0)

set(logging_tps bluestore.tp bluestore_gc.tp dummy.tp bluestore_cache.tp bluefs.tp)
add_tracing_library(logging_tp "${logging_tps}" 1.0.0)

install(TARGETS rados_tp osd_tp ceph_logging_tp logging_tp os_tp rgw_rados_tp rgw_op_tp DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(WITH_RBD)
  add_tracing_library(rbd_tp librbd.tp 1.0.0)
  install(TARGETS rbd_tp DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif(WITH_RBD)
if(WITH_OSD_INSTRUMENT_FUNCTIONS)
  add_tracing_library(cyg_profile_tp cyg_profile.tp 1.0.0)
  install(TARGETS cyg_profile_tp DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if(WITH_LTTNG AND WITH_EVENTTRACE)
  add_tracing_library(eventtrace_tp eventtrace.tp 1.0.0)
  install(TARGETS eventtrace_tp DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

