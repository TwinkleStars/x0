if(ENABLE_X0D)
if(ENABLE_XZERO_FLOW)

add_definitions(
  -pthread
  -std=c++0x
  -DPACKAGE_NAME="${PACKAGE_NAME}"
  -DPACKAGE_VERSION="${PACKAGE_VERSION}"
  -DPACKAGE_HOMEPAGE_URL="${PACKAGE_HOMEPAGE_URL}"
  -DSYSCONFDIR="${SYSCONFDIR}"
  -DLOGDIR="${LOGDIR}"
  -DPLUGINDIR="${PLUGINDIR}"
  -DVERSION="${PACKAGE_VERSION}"
)

set(x0d_SRC
  XzeroCore.cpp
  XzeroDaemon.cpp
  XzeroEventHandler.cpp
  XzeroPlugin.cpp
  x0d.cpp
)

add_executable(x0d ${x0d_SRC})
target_link_libraries(x0d XzeroBase XzeroHttp XzeroFlow ${TCMALLOC_LIBRARIES})

install(TARGETS x0d RUNTIME DESTINATION sbin)
install(FILES x0d.conf-dist DESTINATION ${SYSCONFDIR})

endif(ENABLE_XZERO_FLOW)
endif(ENABLE_X0D)
