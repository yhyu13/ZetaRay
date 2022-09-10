function(CompileHLSL HLSL_PATH ALL_INCLUDES RET)
	# figure out if shader is a compute shader or a vs-ps shader
	get_filename_component(FILE_NAME_WO_EXT ${HLSL_PATH} NAME_WLE)
	set(RE_CS "\\[numthreads.*\\][ \t\r\n]*void[ \t\r\n]+([a-zA-Z][A-Za-z0-9_]*)")
	file(STRINGS "${HLSL_PATH}" DATA NEWLINE_CONSUME)
	string(REGEX MATCH ${RE_CS} MATCH ${DATA})

	# dxc compiler
	find_program(DXC dxc PATHS "${ZETA_DXC_BIN_DIR}" REQUIRED NO_DEFAULT_PATH)

	# compute shader
	if(${CMAKE_MATCH_COUNT} GREATER 0)
		set(MAIN_FUNC ${CMAKE_MATCH_1})
		set(CSO_PATH_DBG "${ZETA_CSO_DIR_DEBUG}/${FILE_NAME_WO_EXT}_cs.cso")
		set(CSO_PATH_RLS "${ZETA_CSO_DIR_RELEASE}/${FILE_NAME_WO_EXT}_cs.cso")

		add_custom_command(
			OUTPUT ${CSO_PATH_DBG} ${CSO_PATH_RLS}
			COMMAND ${DXC} -Qembed_debug -Qstrip_reflect -nologo -Zi -Od -all_resources_bound -enable-16bit-types -WX -T cs_6_6 -E ${MAIN_FUNC} -Fo ${CSO_PATH_DBG} ${HLSL_PATH}
			COMMAND ${DXC} -Qstrip_reflect -nologo -all_resources_bound -enable-16bit-types -WX -T cs_6_6 -E ${MAIN_FUNC} -Fo ${CSO_PATH_RLS} ${HLSL_PATH}
			DEPENDS ${ALL_INCLUDES} "${HLSL_PATH}"
			COMMENT "Compiling HLSL source file ${FILE_NAME_WO_EXT}.hlsl..."
			VERBATIM)

		set(CSOS ${CSO_PATH_DBG} ${CSO_PATH_RLS})
	# VS-PS
	else()
		# vertex shader
		set(CSO_PATH_VS_DBG ${ZETA_CSO_DIR_DEBUG}/${FILE_NAME_WO_EXT}_vs.cso)
		set(CSO_PATH_VS_RLS ${ZETA_CSO_DIR_RELEASE}/${FILE_NAME_WO_EXT}_vs.cso)
		# pixel shader
		set(CSO_PATH_PS_DBG ${ZETA_CSO_DIR_DEBUG}/${FILE_NAME_WO_EXT}_ps.cso)
		set(CSO_PATH_PS_RLS ${ZETA_CSO_DIR_RELEASE}/${FILE_NAME_WO_EXT}_ps.cso)
		
		add_custom_command(
			OUTPUT ${CSO_PATH_VS_DBG} ${CSO_PATH_PS_DBG} ${CSO_PATH_VS_RLS} ${CSO_PATH_PS_RLS}
			COMMAND ${DXC} -Qembed_debug -Qstrip_reflect -nologo -Zi -Od -all_resources_bound -enable-16bit-types -WX -T vs_6_6 -E mainVS -Fo ${CSO_PATH_VS_DBG} ${HLSL_PATH}
			COMMAND ${DXC} -Qembed_debug -Qstrip_reflect -nologo -Zi -Od -all_resources_bound -enable-16bit-types -WX -T ps_6_6 -E mainPS -Fo ${CSO_PATH_PS_DBG} ${HLSL_PATH}
			COMMAND ${DXC} -Qstrip_reflect -nologo -all_resources_bound -enable-16bit-types -WX -T vs_6_6 -E mainVS -Fo ${CSO_PATH_VS_RLS} ${HLSL_PATH}
			COMMAND ${DXC} -Qstrip_reflect -nologo -all_resources_bound -enable-16bit-types -WX -T ps_6_6 -E mainPS -Fo ${CSO_PATH_PS_RLS} ${HLSL_PATH}
			DEPENDS ${ALL_INCLUDES} "${HLSL_PATH}"
			COMMENT "Compiling HLSL source file ${FILE_NAME_WO_EXT}.hlsl..."
			VERBATIM)		

		set(CSOS ${CSO_PATH_VS_DBG} ${CSO_PATH_VS_RLS} ${CSO_PATH_PS_DBG} ${CSO_PATH_PS_RLS})
	endif()

	set(${RET} ${CSOS} PARENT_SCOPE)
endfunction()