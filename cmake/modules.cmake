
if(DEFINED MODULES)
  return()
endif()

set(MODULES
  auloop
  autotest
  b2bua
  intercom
  kaoptions
  vidloop
  aulatency
)

if(DEFINED EXTRA_MODULES)
  list(APPEND MODULES ${EXTRA_MODULES})
endif()
