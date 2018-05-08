add_definitions(
	-DXML_STATIC 
	-DFD_SETSIZE=1024 
	-DNOWINMESSAGES 
	-DHAVE_WOMBAT_MSG 
	-DREFRESH_TRANSPORT 
	-D_CRT_SECURE_NO_WARNINGS 
	-D_CRT_NONSTDC_NO_WARNING
	-D_WINSOCK_DEPRECATED_NO_WARNINGS
	-D_SILENCE_TR1_NAMESPACE_DEPRECATION_WARNING
)

# Force to always compile with W4
string(REGEX REPLACE "/W3" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REGEX REPLACE "/W3" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

set (CMAKE_SHARED_LIBRARY_PREFIX "lib")
set (CMAKE_IMPORT_LIBRARY_PREFIX "lib")

if (CMAKE_BUILD_TYPE MATCHES DEBUG)
	set (CMAKE_SHARED_LIBRARY_SUFFIX "mdd.dll")
	set (CMAKE_IMPORT_LIBRARY_SUFFIX "mdd.lib")
else ()
    set (CMAKE_SHARED_LIBRARY_SUFFIX "md.dll")
	set (CMAKE_IMPORT_LIBRARY_SUFFIX "mdd.lib")
endif ()